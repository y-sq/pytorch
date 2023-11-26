import warnings
from contextlib import nullcontext
from typing import Any, Callable, List, Tuple, Union
from unittest.mock import patch

import torch
import torch.fx.traceback as fx_traceback
import torch.utils._pytree as pytree
from torch import Tensor
from torch._decomp.decompositions_for_rng import PhiloxStateTracker
from torch._guards import detect_fake_mode
from torch._prims_common import CUDARngStateHelper
from torch._subclasses.functional_tensor import FunctionalTensorMode
from torch.fx.experimental.symbolic_shapes import definitely_false, sym_eq

from .. import config
from .functional_utils import (
    from_functional,
    is_functional,
    sync_functional_tensor,
    to_functional,
)
from .logging_utils import setup_stacktrace_preservation_hooks
from .schemas import AOTConfig, MutationType, OutputType, ViewAndMutationMeta
from .utils import maybe_to_fresh_input


# This function returns a new function that returns mutated inputs as outputs.
# if keep_data_input_mutations is set, then we assume that data-only mutations
# will be left in the graph, and we only return metadata-mutated inputs as outputs.
def fn_input_mutations_to_outputs(
    fn: Callable,
    meta: ViewAndMutationMeta,
    keep_data_input_mutations: bool,
) -> Any:
    def inner_fn(*args):
        outs = fn(*args)
        assert len(meta.output_info) == len(outs)
        # The compiled fw will return mutated input tensors, *including* metadata-only mutation.
        # However, if keep_data_input_mutations is set, the compiled fw only needs to return metadata-mutated inputs.
        # (because data-only input mutations are handled directly in the compiled graph)
        mutated_inputs_to_return = [
            x for (i, x) in enumerate(args) if i in meta.mutated_inp_runtime_indices
        ]
        return *mutated_inputs_to_return, *outs

    return inner_fn


# This function takes in a fn with external aliasing and mutation,
# and returns a new fn with no external aliasing and mutation,
# as needed for autograd.
# The main transformations are:
# - Return mutated inputs as extra outputs
# - Clone mutated inputs that require gradients,
#   because autograd will require us to pass the pre-mutated inputs into autograd.grad
# - Return intermediate bases of outputs as additional outputs,
#   needed to appease autograd.Function
# The new function returns:
# (1) The updated outputs
# (2) A boolean mask of len(new_fn_outputs),
#     that can be used to tell autograd.grad which outputs should get tangents
#     if we trace the backward.
def fn_prepped_for_autograd(
    fn: Callable,
    meta: ViewAndMutationMeta,
) -> Any:
    def inner_fn(*args):
        args_maybe_cloned = [
            maybe_to_fresh_input(i, t, meta) for i, t in enumerate(args)
        ]

        outs = fn(*args_maybe_cloned)
        assert isinstance(outs, (tuple, list))
        outs = list(outs)
        assert len(meta.output_info) == len(outs)

        mutated_inputs_to_return = [
            x
            for (i, x) in enumerate(args_maybe_cloned)
            if i in meta.mutated_inp_runtime_indices
        ]

        intermediate_bases = []
        for i, (o, info) in enumerate(zip(outs, meta.output_info)):
            if info.output_type == OutputType.alias_of_intermediate_save_as_output:
                intermediate_bases.append(o._base)

        assert meta.num_intermediate_bases == len(intermediate_bases)

        # the compiled forward should return (mutated_inputs, user_outs, intermediate_bases)
        fw_outs_to_return = *mutated_inputs_to_return, *outs, *intermediate_bases

        # Also return a boolean mask specifying which outputs to this function will be used as tangents
        mutated_inputs_grad_mask = [
            meta.input_info[meta.mutated_inp_runtime_indices[i]].mutates_data
            and meta.input_info[meta.mutated_inp_runtime_indices[i]].requires_grad
            for (i, x) in enumerate(mutated_inputs_to_return)
        ]

        # Pass any (non-aliased) outputs in as tangents, since they'll be returned as outputs in the fw
        # For outputs that are aliases of intermediates, we will have returned the output's _base as an output in the graph instead,
        # which we *should* send to grad()
        output_grad_mask = [
            meta.output_info[i].output_type
            in [
                OutputType.non_alias,
                OutputType.unsafe_view_alias,
                OutputType.custom_function_view,
            ]
            # Also, only tensor outputs should participate in the backward
            # (in particular, Symint outputs in the forward graph shouldn't get tangents)
            and issubclass(meta.output_info[i].raw_type, Tensor)
            and meta.output_info[i].requires_grad
            for (i, x) in enumerate(outs)
        ]

        intermediate_base_grad_mask = [True for _ in range(len(intermediate_bases))]

        out_grad_mask = (
            mutated_inputs_grad_mask + output_grad_mask + intermediate_base_grad_mask
        )
        assert len(out_grad_mask) == len(fw_outs_to_return)

        # Take care to grab and sync the updated inputs from primals_after_cloning (the inputs we actually mutate!)
        # and not primals (the preserved inputs, pre-mutation, that we pass to grad())
        # This is annoying: our joint function needs to be aware of functionalization
        # (syncing mutated inputs before calling autograd.grad())
        # In theory, we could make the autograd engine do this automatically, although that probably isn't any cleaner.
        for arg in args_maybe_cloned:
            if not isinstance(arg, Tensor):
                continue
            sync_functional_tensor(arg)

        return fw_outs_to_return, out_grad_mask

    return inner_fn


# Given a fn, computes the joint.
# NOTE: fn is expects the following behavior:
# (1) fn() needs to return a tuple of (outs, mask),
#     where `mask` tells us which outputs are meant to have tangents.
#     we don't know this info automatically, because we don't actually want to blindly
#     compute tangents for every output that requires grad.
#     Specifically, outputs that alias inputs won't participate in the backward and get tangents.
# (2) fn() cannot mutate any inputs that require gradient.
#     otherwise, when we compute autograd.grad(), we will not take those input mutations into account
#     (the way this is handled is that we ensure any inputs that normally get mutated are cloned first)
def create_joint(fn: Callable, *, aot_config: AOTConfig) -> Any:
    def inner_fn(primals: List[Any], tangents: List[Any]):
        outs, tangent_mask = fn(*primals)
        assert len(tangent_mask) == len(outs)
        outs_to_grad = [
            o for needs_tangent, o in zip(tangent_mask, outs) if needs_tangent
        ]
        assert len(outs_to_grad) == len(tangents)

        # Get the inputs that need gradients
        grad_primals = []
        inputs_needs_grads = []
        # Note that we're not using primals here,
        # being carefully not to pass any mutated inputs into autograd.grad()
        for p in primals:
            is_grad_tensor = isinstance(p, Tensor) and p.requires_grad
            inputs_needs_grads.append(is_grad_tensor)
            if is_grad_tensor:
                grad_primals.append(p)

        # Get the outputs that need gradients
        needed_outs = []
        needed_tangents = []
        for out, tangent in zip(outs_to_grad, tangents):
            if isinstance(out, Tensor) and out.requires_grad:
                # A bit sketchy, but fixes e.g. test_aot_autograd_exhaustive_matmul_cpu_float32
                # The issue is that we are sensitive to decomps that don't accurately maintain
                # their output's _base.shape compared to eager mode, and this helps mitigate a bit.
                # The not definitely_false is also sketchy; if unbacked
                # symints are involved, we're just going to assume that the
                # decomps setup the base shape correctly
                needed_outs.append(
                    out
                    if not definitely_false(sym_eq(out.shape, tangent.shape))
                    else out.view(tangent.shape)
                )
                needed_tangents.append(tangent)

        setup_stacktrace_preservation_hooks([out.grad_fn for out in needed_outs])

        if config.functionalize_rng_ops:
            PhiloxStateTracker.mark_beginning_of_backward()
        backward_out: Tuple[Tensor, ...] = tuple()
        # Call the backwards pass
        if grad_primals:
            with fx_traceback.preserve_node_meta():
                # for full graph export, we always export a joint graph where we assume no tangents are needed.
                if aot_config.no_tangents:
                    assert len(needed_tangents) == 1 and needed_tangents[0].numel() == 1
                    backward_out = torch.autograd.grad(
                        needed_outs,
                        grad_primals,
                        allow_unused=True,
                    )
                else:
                    backward_out = torch.autograd.grad(
                        needed_outs,
                        grad_primals,
                        grad_outputs=needed_tangents,
                        allow_unused=True,
                    )
        backward_out_iter = iter(backward_out)
        return outs, [
            next(backward_out_iter) if i else None for i in inputs_needs_grads
        ]

    def inner_fn_with_anomaly(*args):
        with fx_traceback.preserve_node_meta(), warnings.catch_warnings():
            warnings.filterwarnings("ignore", "Anomaly Detection has been enabled.")
            with torch.autograd.detect_anomaly(check_nan=False):
                return inner_fn(*args)

    return inner_fn_with_anomaly


def _create_functionalized_rng_ops_wrapper(func, args, trace_joint=True) -> Any:
    # Functionalization of rng ops changes the calling convention of the joint graph.
    # It goes from (primals, tangents) to (seed, offset, primals, tangents)
    # At runtime, we pass on the current seed and offset. This is hidden from
    # the user.
    fake_mode = detect_fake_mode()
    if fake_mode is None:
        fake_mode = nullcontext()

    def override_get_rng_state(device: Union[int, str, torch.device] = "cuda"):
        out = PhiloxStateTracker.get_state_as_tensor()
        return out

    def override_set_rng_state(x, device: Union[int, str, torch.device] = "cuda"):
        PhiloxStateTracker.set_state_from_tensor(x)

    def append_rng_offsets(args):
        if trace_joint:
            # args signature before: Tuple(fwd_outputs), Tuple(bwd_outputs)
            # args signature after: Tuple(fwd_outputs, new_fwd_rng_offset), Tuple(bwd_offset, new_bwd_rng_offset)
            return (
                (*args[0], PhiloxStateTracker.get_updated_fwd_offset()),
                (*args[1], PhiloxStateTracker.get_updated_bwd_offset()),
            )
        else:
            # args signature before: Tuple(fwd_outputs)
            # args signature after: Tuple(fwd_outputs, new_fwd_rng_offset)
            return (*args, PhiloxStateTracker.get_updated_fwd_offset())

    def traced_joint(
        primals, tangents, fwd_seed, fwd_base_offset, bwd_seed, bwd_base_offset
    ):
        with patch("torch.cuda.get_rng_state", override_get_rng_state), patch(
            "torch.cuda.set_rng_state", override_set_rng_state
        ):
            return append_rng_offsets(func(primals, tangents))

    def traced_forward(*primals_fwd_seed_fwd_base_offset):
        # The signature is (*primals, seed, offset)
        with patch("torch.cuda.get_rng_state", override_get_rng_state), patch(
            "torch.cuda.set_rng_state", override_set_rng_state
        ):
            return append_rng_offsets(func(*primals_fwd_seed_fwd_base_offset[:-2]))

    if trace_joint:
        # Get the current seed and offset to setup tracing.
        fwd_seed, fwd_base_offset = CUDARngStateHelper.get_torch_state_as_tuple(
            fake_mode
        )
        bwd_seed, bwd_base_offset = CUDARngStateHelper.get_torch_state_as_tuple(
            fake_mode
        )
        PhiloxStateTracker.record_state(fwd_seed, fwd_base_offset, "forward")
        PhiloxStateTracker.record_state(bwd_seed, bwd_base_offset, "backward")
        return traced_joint, (
            *args,
            fwd_seed,
            fwd_base_offset,
            bwd_seed,
            bwd_base_offset,
        )
    else:
        # Get the current seed and offset to setup tracing.
        fwd_seed, fwd_base_offset = CUDARngStateHelper.get_torch_state_as_tuple(
            fake_mode
        )
        PhiloxStateTracker.record_state(fwd_seed, fwd_base_offset, "forward")
        return traced_forward, (*args, fwd_seed, fwd_base_offset)


# This creates the final function that we want to trace using make_fx(),
# in both aot_dispatch_autograd and aot_dispatch_base.
# Preconditions:
# - fn corresponds to the user's fw function
# - fn arguments have been flattened, duplicate arguments have been handled
# - In the returned function, the "primals" arguments *includes* synthetic bases.
# This function does the work of functionalizing the input function,
# and performing copy_() calls at the end of the function if `keep_input_mutations` is set.
# The function returned has signature that is either:
# (1) "traced_fn(primals: List[Any])" if trace_joint is False
# (2) "traced_fn(primals: List[Any], tangents: List[Any])" if trace_joint is True
# Returns a new (functionalized) function, and updated arguments to call it with.
def create_functionalized_fn(
    fn,
    args,
    *,
    meta: ViewAndMutationMeta,
    aot_config: AOTConfig,
    trace_joint: bool,
) -> Any:
    def _functionalized_f_helper(*args):
        # Wrap inputs into functional wrappers
        f_args = pytree.tree_map(to_functional, args)

        # See Note [Disabling Functionalize TLS Above Python Functionalization]
        disable_above = torch._C._ExcludeDispatchKeyGuard(
            torch._C.DispatchKeySet(torch._C.DispatchKey.Functionalize)
        )
        with disable_above, FunctionalTensorMode():
            # Run the joint
            f_outs = fn(*f_args)

        if aot_config.keep_inference_input_mutations:
            # Note: This is a bit annoying. There's a layering issue here, where:
            # (1) functionalization needs to operate on **synthetic base** inputs, before unpacking them into the "real" inputs.
            # (2) For keep_input_mutations, we support tracing a call to copy_() directly on mutated inputs.
            #     However, we **only** want to support this for inputs that have data-only (and no metadata) mutations,
            #     because inductor (and backends in generally) would prefer not to see these (e.g. as_strided_(), resize_()).
            #     This makes it pretty difficult for this logic to operate on synthetic bases.
            # (3) In addition, there are cases where it's significantly cheaper to perform the copy on the individual
            #     (unpacked) input aliases, instead of the synthetic base.
            # Example case where (3) could be important:
            #
            #     def f(x, y):
            #         x.mul_(2)
            #         y.mul_(3)
            #         return x, y
            #    a = torch.ones(1'000'000)
            #    x, y = out(a[0:9], a[1:10])
            #
            # It would be much better to add copy_() calls into the graph for the two tiny slices, instead of materializing
            # a giant "updated synthetic base" and copying into a's entire storage.
            #
            # For now, we are pessimistically not performing the optimization from (3);
            # we will materialize an "updated" synthetic base, and copy it back to the synthetic input base.
            # This allows us to factor aot autograd much more nicely, since only one area of the code needs to worry
            # about synthetic bases.
            for i, (inpt_old, inpt_f) in enumerate(
                zip(args, f_args) if not trace_joint else zip(args[0], f_args[0])
            ):
                if not isinstance(inpt_f, torch.Tensor):
                    continue
                assert is_functional(inpt_f)
                inpt_new = from_functional(inpt_f)
                if meta.input_info[i].mutation_type == MutationType.MUTATED_IN_GRAPH:
                    # We found an input that had a (data-only) mutation.
                    # Since keep_input_mutations is set, we need to faithfully apply a copy_()
                    # so the compiler will see the input mutation in the graph.
                    if meta.input_info[i].mutations_hidden_from_autograd:
                        with torch.no_grad(), torch.autograd._unsafe_preserve_version_counter(
                            inpt_old
                        ):
                            inpt_old.copy_(inpt_new)
                    else:
                        inpt_old.copy_(inpt_new)

        return pytree.tree_map(from_functional, f_outs)

    # Kinda annoying, but needed to make sure that the fx graph we trace out has "primals"
    # and "tangents" as its input names (which are special-cased by the partitioner)
    def joint_helper(primals, tangents):
        return _functionalized_f_helper(primals, tangents)

    def fwd_helper(*args):
        return _functionalized_f_helper(*args)

    helper = joint_helper if trace_joint else fwd_helper
    if config.functionalize_rng_ops:
        # Setup the wrapper for functionalization of rng ops
        helper, args = _create_functionalized_rng_ops_wrapper(helper, args, trace_joint)

    return helper, args

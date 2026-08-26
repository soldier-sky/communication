# E2E Detailed Design (Events/Fields)

> Note: Draft. This file currently covers SWP-279039 (single-sample Protect/Check
> diagrams) only. Windowed supervision state machine (SWP-279114) and the full
> wrapping narrative/traceability table migrated from ticket SWP-273562
> mw_com_e2e_binding.md, (SWP-279129) are tracked
> separately and will supersede this scaffold.

## Introduction

E2E protection in `mw::com` separates two responsibilities: generation/verification
of E2E protection information (Protect/Check, this section) and interpretation of
check results over time (windowed state machine, see SWP-279114). This document covers
Events/Fields only; method request/response E2E is out of scope (see
[mw_com_e2e_binding.md](SWP-273562)).

## Single-Sample Protect/Check Design

### Context / Layering

![E2E Context Diagram](/score/mw/com/dependability/software_architectural_design/e2e/assets/e2e_context_diagram.puml)

The binding layer is stateless with respect to E2E communication supervision: it only
places/extracts precomputed header bytes at its transport-specific offset. Both
`E2E_PXXProtect()` and `E2E_PXXCheck()` are invoked exclusively by the
binding-independent layer.

### Sender: Protect on `Send()`

![E2E Skeleton Protect Sequence](score/mw/com/dependability/software_architectural_design/e2e/assets/e2e_skeleton_protect_sequence.puml)

`SkeletonEventBase` owns exactly one `E2EProtectContext` per event, shared across all
consumers (no per-subscriber replication — a per-consumer counter would require a
distinct frame per subscriber, defeating multicast delivery). `E2E_PXXProtect()` is
invoked before the binding's `Send()`, and the binding receives already-computed
header bytes rather than computing them itself.

Satisfies: `SkeletonEventOwnsE2EProtectContext`, `E2EProtectContextNotReplicatedPerConsumer`,
`E2EProtectionInvokedBeforeBindingSend`, `BindingReceivesPrecomputedE2EHeader`.

### Receiver: Check on `GetNewSamples()`

![E2E Proxy Check Sequence](score/mw/com/dependability/software_architectural_design/e2e/assets/e2e_proxy_check_sequence.puml)

`ProxyEventBase` owns one E2E check context per consuming instance, independent of
any other consumer of the same event. The check runs on every `GetNewSamples()` call,
whether or not a new sample arrived — a cycle with no new sample feeds an explicit
"no new data" outcome into the check rather than being skipped, since an application
must treat "no new data" as the absence of a trustworthy sample, not an implicit pass.

Satisfies: `ProxyEventOwnsE2ECheckContext`, `E2ESupervisionInvokedOnEveryGetNewSamplesCall`,
`NoNewSampleFedAsNoNewDataToE2ECheck`, `IndependentE2ESupervisionPerConsumer`. Corroborated
by AoU `ApplicationMustTreatNoNewDataAsUntrustworthy`.

## Windowed Supervision State Machine

> Tracked in SWP-279114 — not yet part of this document.

## Traceability

> Full traceability table across all E2E CompReqs/FeatReqs/AoUs tracked in SWP-279129.

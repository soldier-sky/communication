/********************************************************************************
 * Copyright (c) 2025 Contributors to the Eclipse Foundation
 *
 * See the NOTICE file(s) distributed with this work for additional
 * information regarding copyright ownership.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Apache License Version 2.0 which is available at
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * SPDX-License-Identifier: Apache-2.0
 ********************************************************************************/
#ifndef SCORE_MW_COM_IMPL_BINDINGS_LOLA_PROXY_EVENT_H
#define SCORE_MW_COM_IMPL_BINDINGS_LOLA_PROXY_EVENT_H

#include "score/mw/com/impl/bindings/lola/event_data_storage.h"
#include "score/mw/com/impl/bindings/lola/event_meta_info.h"
#include "score/mw/com/impl/bindings/lola/proxy_event_common.h"
#include "score/mw/com/impl/configuration/e2e_event_type_deployment.h"
#include "score/mw/com/impl/e2e/e2e_profile_stub.h"

#include "score/language/safecpp/safe_math/safe_math.h"
#include "score/memory/shared/pointer_arithmetic_util.h"
#include "score/mw/com/impl/proxy_event_binding.h"
#include "score/mw/com/impl/sample_reference_tracker.h"
#include "score/mw/com/impl/subscription_state.h"
#include "score/mw/com/impl/tracing/i_tracing_runtime.h"

#include "score/mw/log/logging.h"
#include "score/result/result.h"

#include <score/assert.hpp>

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <variant>

namespace score::mw::com::impl::lola
{

/// \brief Proxy event binding implementation for the Lola IPC binding.
///
/// All subscription operations are implemented in the separate class SubscriptionStateMachine and the associated
/// states. All type agnostic proxy event operations are dispatched to the class ProxyEventCommon.
///
/// \tparam SampleType Data type that is transmitted
template <typename SampleType>
class ProxyEvent final : public ProxyEventBinding<SampleType>
{
    template <typename T>
    // coverity[autosar_cpp14_a11_3_1_violation] friend to test class; is used to access proxy_event_common_
    friend class ProxyEventAttorney;

  public:
    using typename ProxyEventBinding<SampleType>::Callback;

    ProxyEvent() = delete;
    /// Create a new instance that is bound to the specified ShmBindingInformation and ElementId.
    ///
    /// \param parent Parent proxy of the proxy event.
    /// \param element_fq_id The ID of the event inside the proxy type.
    /// \param event_name The name of the event inside the proxy type.
    /// \param e2e_event_deployment The event's POC E2E deployment, resolved from the same LolaServiceTypeDeployment
    ///        the provider resolves its own copy from; empty when the event has no configured E2E profile.
    /// \param e2e_header_storage Scaffolding-only per-slot POC header storage shared with the provider's
    ///        SkeletonEventCommon by the harness that wires them together; null when E2E is not configured.
    ProxyEvent(Proxy& parent,
              const ElementFqId element_fq_id,
              const std::string_view event_name,
              std::optional<E2EEventTypeDeployment> e2e_event_deployment = {},
              std::shared_ptr<e2e::HeaderStorage> e2e_header_storage = nullptr)
        : ProxyEventBinding<SampleType>{},
          proxy_event_common_{parent, element_fq_id, event_name},
          meta_info_{parent.GetEventMetaInfo(element_fq_id)},
          aligned_sample_size_{memory::shared::CalculateAlignedSize(sizeof(SampleType), alignof(SampleType))},
          event_slots_raw_array_{InitialiseEventSlotsRawArray()},
          e2e_event_deployment_{e2e_event_deployment},
          e2e_header_storage_{std::move(e2e_header_storage)}
    {
        parent.RegisterEvent(event_name, *this);
    }

    ProxyEvent(const ProxyEvent&) = delete;
    ProxyEvent(ProxyEvent&&) noexcept = delete;
    ProxyEvent& operator=(const ProxyEvent&) = delete;
    ProxyEvent& operator=(ProxyEvent&&) noexcept = delete;

    ~ProxyEvent() noexcept override = default;

    Result<void> Subscribe(const std::size_t max_sample_count) noexcept override
    {
        return proxy_event_common_.Subscribe(max_sample_count);
    }
    void Unsubscribe() noexcept override
    {
        proxy_event_common_.Unsubscribe();
    }

    SubscriptionState GetSubscriptionState() const noexcept override
    {
        return proxy_event_common_.GetSubscriptionState();
    }
    Result<std::size_t> GetNumNewSamplesAvailable() const override;
    Result<std::size_t> GetNewSamples(Callback&& receiver, TrackerGuardFactory& tracker) noexcept override;

    Result<void> SetReceiveHandler(std::weak_ptr<ScopedEventReceiveHandler> handler) noexcept override
    {
        return proxy_event_common_.SetReceiveHandler(std::move(handler));
    }
    Result<void> UnsetReceiveHandler() noexcept override
    {
        return proxy_event_common_.UnsetReceiveHandler();
    }
    Result<void> SetSubscriptionStateChangeHandler(SubscriptionStateChangeHandler handler) noexcept override
    {
        return proxy_event_common_.SetSubscriptionStateChangeHandler(std::move(handler));
    }
    Result<void> UnsetSubscriptionStateChangeHandler() noexcept override
    {
        return proxy_event_common_.UnsetSubscriptionStateChangeHandler();
    }
    std::optional<std::uint16_t> GetMaxSampleCount() const noexcept override
    {
        return proxy_event_common_.GetMaxSampleCount();
    }
    BindingType GetBindingType() const noexcept override
    {
        return BindingType::kLoLa;
    }
    void NotifyServiceInstanceChangedAvailability(bool is_available, pid_t new_event_source_pid) noexcept override
    {
        proxy_event_common_.NotifyServiceInstanceChangedAvailability(is_available, new_event_source_pid);
    }

    ElementFqId GetElementFQId() const noexcept
    {
        return proxy_event_common_.GetElementFQId();
    };

    /// \brief Returns this consumer's resolved POC E2E deployment, matching the provider's own resolution of the
    ///        same event from the identical LolaServiceTypeDeployment.
    const std::optional<E2EEventTypeDeployment>& GetE2EEventDeployment() const noexcept
    {
        return e2e_event_deployment_;
    }

    e2e::E2EResult GetLastE2EResult() const noexcept override
    {
        return last_e2e_result_;
    }

  private:
    const std::uint8_t* InitialiseEventSlotsRawArray();

    Result<std::size_t> GetNewSamplesImpl(Callback&& receiver, TrackerGuardFactory& tracker) noexcept;
    Result<std::size_t> GetNumNewSamplesAvailableImpl() const noexcept;

    ProxyEventCommon proxy_event_common_;
    const EventMetaInfo& meta_info_;
    const std::size_t aligned_sample_size_;
    const std::uint8_t* event_slots_raw_array_;
    std::optional<E2EEventTypeDeployment> e2e_event_deployment_;
    std::shared_ptr<e2e::HeaderStorage> e2e_header_storage_;
    e2e::CheckContext e2e_check_context_{};
    e2e::E2EResult last_e2e_result_{};
};

template <typename SampleType>
inline const std::uint8_t* ProxyEvent<SampleType>::InitialiseEventSlotsRawArray()
{
    auto& event_data_control_local = proxy_event_common_.GetConsumerEventDataControlLocal();

    const auto event_slots_raw_array_size = safe_math::Multiply<safe_math::ReturnMode::kAbortOnError>(
        aligned_sample_size_, event_data_control_local.GetMaxSampleSlots());

    const void* const event_slots_raw_array = meta_info_.event_slots_raw_array_.get(event_slots_raw_array_size);

    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(nullptr != event_slots_raw_array, "Null event slot array");
    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(meta_info_.data_type_info_.Size() == sizeof(SampleType),
                                                      "Event sample size mismatch");
    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(meta_info_.data_type_info_.Alignment() == alignof(SampleType),
                                                      "Event sample alignment mismatch");

    return static_cast<const std::uint8_t*>(event_slots_raw_array);
}

template <typename SampleType>
inline Result<std::size_t> ProxyEvent<SampleType>::GetNumNewSamplesAvailable() const
{
    /// In case of LoLa binding we can also dispatch to GetNumNewSamplesAvailableImpl() in case of kSubscriptionPending!
    /// Because a pre-condition to kSubscriptionPending is that we once had a successful subscription... and then we can
    /// always access the samples even if the provider went down.
    const auto subscription_state = proxy_event_common_.GetSubscriptionState();
    if (subscription_state == SubscriptionState::kNotSubscribed)
    {
        return MakeUnexpected(ComErrc::kNotSubscribed,
                              "Attempt to call GetNumNewSamplesAvailable without successful subscription.");
    }
    return GetNumNewSamplesAvailableImpl();
}

template <typename SampleType>
inline Result<std::size_t> ProxyEvent<SampleType>::GetNumNewSamplesAvailableImpl() const noexcept
{
    return proxy_event_common_.GetNumNewSamplesAvailable();
}

template <typename SampleType>
inline Result<std::size_t> ProxyEvent<SampleType>::GetNewSamples(Callback&& receiver,
                                                                 TrackerGuardFactory& tracker) noexcept
{
    /// In case of LoLa binding we can also dispatch to GetNewSamplesImpl() in case of kSubscriptionPending!
    /// Because a pre-condition to kSubscriptionPending is that we once had a successful subscription... and then we can
    /// always access the samples even if the provider went down.
    const auto subscription_state = proxy_event_common_.GetSubscriptionState();
    if (subscription_state == SubscriptionState::kNotSubscribed)
    {
        return MakeUnexpected(ComErrc::kNotSubscribed,
                              "Attempt to call GetNewSamples without successful subscription.");
    }
    return GetNewSamplesImpl(std::move(receiver), tracker);
}

template <typename SampleType>
// Suppress "AUTOSAR C++14 M3-2-2" rule finding. This rule declares: "The One Definition Rule shall not be
// violated.". False-positive, template method is defined only once.
// Suppress "AUTOSAR C++14 A15-5-3" rule findings. This rule states: "The std::terminate() function shall not be called
// implicitly". This is a false positive, all results which are accessed with '.value()' that could implicitly call
// 'std::terminate()' (in case it doesn't have value) has a check in advance using '.has_value()', so no way for
// throwing std::bad_optional_access which leds to std::terminate(). This suppression should be removed after fixing
// [Ticket-173043](broken_link_j/Ticket-173043)
// coverity[autosar_cpp14_m3_2_2_violation : FALSE]
// coverity[autosar_cpp14_a15_5_3_violation : FALSE]
inline Result<std::size_t> ProxyEvent<SampleType>::GetNewSamplesImpl(Callback&& receiver,
                                                                     TrackerGuardFactory& tracker) noexcept
{
    const auto max_sample_count = tracker.GetNumAvailableGuards();
    const auto slot_indices = proxy_event_common_.GetNewSamplesSlotIndices(max_sample_count);

    auto& event_data_control_local = proxy_event_common_.GetConsumerEventDataControlLocal();
    SCORE_LANGUAGE_FUTURECPP_PRECONDITION_PRD_MESSAGE(nullptr != event_slots_raw_array_, "Null event slot array");

    for (auto slot_index_it = slot_indices.begin; slot_index_it != slot_indices.end; ++slot_index_it)
    {
        // TODO: Replace this temporary raw-slot access when the LoLa binding layer is type-erased.
        // The current fix avoids interpreting GenericSkeleton-created storage as EventDataStorage<SampleType>, since
        // the DynamicArray element count may not match the typed proxy sample type.
        // NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic) The pointer event_slots_raw_array_ points to
        // the first byte of the type-erased event sample storage in shared memory. Samples may originate from either a
        // typed SkeletonEvent or a GenericSkeletonEvent, therefore slot lookup must use the stable EventMetaInfo raw
        // storage address and SampleType stride instead of interpreting the shared-memory DynamicArray object type.
        const auto* const object_start_address = &event_slots_raw_array_[aligned_sample_size_ * (*slot_index_it)];
        // NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

        // Suppress "AUTOSAR C++14 M5-2-8" rule finding: "An object with integer type or pointer to void type shall
        // not be converted to an object with pointer type.".
        // The raw storage address is provided through EventMetaInfo. The regular typed proxy validates the expected
        // type at construction time and calculates the slot offset with sizeof(SampleType)/alignof(SampleType).
        // coverity[autosar_cpp14_m5_2_8_violation]
        const SampleType& sample_data{*reinterpret_cast<const SampleType*>(object_start_address)};
        const EventSlotStatus event_slot_status{event_data_control_local[*slot_index_it]};
        const EventSlotStatus::EventTimeStamp sample_timestamp{event_slot_status.GetTimeStamp()};

        last_e2e_result_ = e2e::E2EResult{};
        if (e2e_event_deployment_.has_value() && (e2e_header_storage_ != nullptr) &&
            (*slot_index_it < e2e_header_storage_->size()))
        {
            const e2e::ProfileConfiguration profile_config{e2e_event_deployment_->data_id_,
                                                            e2e_event_deployment_->max_delta_counter_};
            auto& header = (*e2e_header_storage_)[*slot_index_it];
            const score::cpp::span<const std::byte> header_span{header.data(), header.size()};
            // Suppress "AUTOSAR C++14 M5-2-8": payload bytes are read only for the POC checksum.
            // coverity[autosar_cpp14_m5_2_8_violation]
            const score::cpp::span<const std::byte> payload_span{reinterpret_cast<const std::byte*>(&sample_data),
                                                                  sizeof(SampleType)};
            const auto outcome = e2e::CheckMessage(header_span, payload_span, profile_config, e2e_check_context_);
            const bool sequence_ok = (outcome.sequence == e2e::SequenceStatus::kOk) ||
                                     (outcome.sequence == e2e::SequenceStatus::kOkGapWithinThreshold);
            last_e2e_result_.data_integrity = outcome.data_integrity;
            last_e2e_result_.sequence = outcome.sequence;
            last_e2e_result_.historical_health = e2e::HistoricalHealthStatus::kDisabled;
            last_e2e_result_.summary = ((outcome.data_integrity == e2e::DataIntegrityStatus::kOk) && sequence_ok)
                                           ? e2e::Summary::kOk
                                           : e2e::Summary::kError;
            if (last_e2e_result_.summary == e2e::Summary::kError)
            {
                score::mw::log::LogWarn("lola")
                    << "E2E check failed: slot" << *slot_index_it << "data_integrity"
                    << static_cast<std::uint8_t>(outcome.data_integrity) << "sequence"
                    << static_cast<std::uint8_t>(outcome.sequence);
            }
            else
            {
                score::mw::log::LogDebug("lola") << "E2E check passed: slot" << *slot_index_it;
            }
        }

        SamplePtr<SampleType> sample{&sample_data, event_data_control_local, *slot_index_it};

        auto guard = std::move(*tracker.TakeGuard());
        auto sample_binding_independent = this->MakeSamplePtr(std::move(sample), std::move(guard));

        static_assert(
            sizeof(EventSlotStatus::EventTimeStamp) == sizeof(impl::tracing::ITracingRuntime::TracePointDataId),
            "Event timestamp is used for the trace point data id, therefore, the types should be the same.");
        // Suppress "AUTOSAR C++14 A15-4-2" rule finding. This rule states: "I a function is declared to be
        // noexcept, noexcept(true) or noexcept(<true condition>), then it shall not exit with an exception"
        // we can't add noexcept to score::cpp::callback signature.
        // coverity[autosar_cpp14_a15_4_2_violation]
        receiver(std::move(sample_binding_independent),
                 static_cast<impl::tracing::ITracingRuntime::TracePointDataId>(sample_timestamp));
    }

    const auto num_collected_slots = static_cast<std::size_t>(std::distance(slot_indices.begin, slot_indices.end));
    return num_collected_slots;
}

}  // namespace score::mw::com::impl::lola

#endif  // SCORE_MW_COM_IMPL_BINDINGS_LOLA_PROXY_EVENT_H

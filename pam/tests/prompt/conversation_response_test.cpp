#include "prompt/conversation_response.hpp"
#include "prompt/conversation_response/internal.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <span>

#include <string.h>

namespace {
	struct Event {
		void       *pointer = nullptr;
		std::size_t length  = 0;
		bool        erase   = false;
	};

	struct Context {
		std::array<Event, 16> events{};
		std::size_t           count = 0;
	};

	void RecordErase(void *context, std::span<char> bytes) noexcept {
		auto &state                    = *static_cast<Context *>(context);
		state.events.at(state.count++) = {
		    .pointer = bytes.data(), .length = bytes.size(), .erase = true};
		explicit_bzero(bytes.data(), bytes.size());
	}

	void RecordRelease(howdy::pam::detail::ConversationResponseAllocation allocation) noexcept {
		auto &state                    = *static_cast<Context *>(allocation.context);
		state.events.at(state.count++) = {.pointer = allocation.pointer, .erase = false};
		std::free(allocation.pointer);
	}

	auto MakeResponses(std::initializer_list<const char *> values) -> struct pam_response * {
		auto *responses = static_cast<struct pam_response *>(
		    std::calloc(values.size(), sizeof(struct pam_response)));
		if (responses == nullptr) {
			return nullptr;
		}
		auto response_view = std::span(responses, values.size());
		std::ranges::transform(
		    values, response_view.begin(), [](const char *value) -> pam_response {
			    return pam_response{.resp         = value == nullptr ? nullptr : strdup(value),
				                    .resp_retcode = 0};
		    });
		return responses;
	}

	auto TestCleanup(std::initializer_list<const char *> values) -> bool {
		Context context;
		auto   *responses = MakeResponses(values);
		howdy::pam::detail::SecureFreeConversationResponses(
		    &responses, static_cast<int>(values.size()),
		    {.context = &context, .erase = RecordErase, .release = RecordRelease});
		bool        ok = howdy::test::Expect(responses == nullptr, "cleanup nulls caller pointer");
		std::size_t strings = 0;
		for (const char *value : values) {
			if (value != nullptr) {
				const auto erase_event   = context.events.at(strings * 2);
				const auto release_event = context.events.at((strings * 2) + 1);
				ok &= howdy::test::Expect(
				    erase_event.erase && erase_event.length == std::strlen(value) &&
				        !release_event.erase && release_event.pointer == erase_event.pointer,
				    "response erased before release");
				++strings;
			}
		}
		return ok && howdy::test::Expect(context.count == (strings * 2) + 1,
		                                 "each response and array released once");
	}
}  // namespace

auto main() -> int {
	bool                 ok        = true;
	struct pam_response *responses = nullptr;
	howdy::pam::SecureFreeConversationResponses(&responses, 0);
	howdy::pam::SecureFreeConversationResponses(nullptr, 0);
	ok &= TestCleanup({""});
	ok &= TestCleanup({"secret"});
	ok &= TestCleanup({"one", "two"});
	ok &= TestCleanup({"partial", nullptr, "last"});
	ok &= TestCleanup({nullptr, nullptr});
	return ok ? 0 : 1;
}

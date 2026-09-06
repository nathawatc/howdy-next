#include "prompt/conversation_response/internal.hpp"
#include "prompt/conversation_response.hpp"
#include "test_support.hpp"

#include <array>
#include <cstdlib>
#include <cstring>

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

	void record_erase(howdy::pam::detail::ConversationResponseAllocation allocation,
	                  std::size_t                                        length) noexcept {
		auto &state                 = *static_cast<Context *>(allocation.context);
		state.events[state.count++] = {
		    .pointer = allocation.pointer, .length = length, .erase = true};
		explicit_bzero(allocation.pointer, length);
	}

	void record_release(howdy::pam::detail::ConversationResponseAllocation allocation) noexcept {
		auto &state                 = *static_cast<Context *>(allocation.context);
		state.events[state.count++] = {.pointer = allocation.pointer, .erase = false};
		std::free(allocation.pointer);
	}

	auto make_responses(std::initializer_list<const char *> values) -> struct pam_response * {
		auto *responses = static_cast<struct pam_response *>(
		    std::calloc(values.size(), sizeof(struct pam_response)));
		std::size_t index = 0;
		for (const char *value : values) {
			responses[index++].resp = value == nullptr ? nullptr : strdup(value);
		}
		return responses;
	}

	auto test_cleanup(std::initializer_list<const char *> values) -> bool {
		Context context;
		auto   *responses = make_responses(values);
		howdy::pam::detail::secure_free_conversation_responses(
		    &responses, static_cast<int>(values.size()),
		    {.context = &context, .erase = record_erase, .release = record_release});
		bool        ok = howdy::test::expect(responses == nullptr, "cleanup nulls caller pointer");
		std::size_t strings = 0;
		for (const char *value : values) {
			if (value != nullptr) {
				const auto erase_event   = context.events.at(strings * 2);
				const auto release_event = context.events.at((strings * 2) + 1);
				ok &= howdy::test::expect(
				    erase_event.erase && erase_event.length == std::strlen(value) &&
				        !release_event.erase && release_event.pointer == erase_event.pointer,
				    "response erased before release");
				++strings;
			}
		}
		return ok && howdy::test::expect(context.count == (strings * 2) + 1,
		                                 "each response and array released once");
	}
}  // namespace

auto main() -> int {
	bool                 ok        = true;
	struct pam_response *responses = nullptr;
	howdy::pam::secure_free_conversation_responses(&responses, 0);
	howdy::pam::secure_free_conversation_responses(nullptr, 0);
	ok &= test_cleanup({""});
	ok &= test_cleanup({"secret"});
	ok &= test_cleanup({"one", "two"});
	ok &= test_cleanup({"partial", nullptr, "last"});
	ok &= test_cleanup({nullptr, nullptr});
	return ok ? 0 : 1;
}

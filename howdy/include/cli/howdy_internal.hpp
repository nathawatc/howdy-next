#pragma once

#include <string>

#include <sys/types.h>

namespace howdy::native::howdy_internal {

	using CommandMain = int (*)(int argc, char **argv);

	struct HowdyDependencies {
		void *context                              = nullptr;
		std::string (*resolve_user)(void *context) = nullptr;
		uid_t (*effective_uid)(void *context)      = nullptr;
		CommandMain add                            = nullptr;
		CommandMain clear                          = nullptr;
		CommandMain config                         = nullptr;
		CommandMain disable                        = nullptr;
		CommandMain download_models                = nullptr;
		CommandMain list                           = nullptr;
		CommandMain remove                         = nullptr;
		CommandMain set                            = nullptr;
		CommandMain snapshot                       = nullptr;
		CommandMain test                           = nullptr;
	};

	int howdy_main_with_dependencies(int argc, char **argv, const HowdyDependencies &dependencies);

}  // namespace howdy::native::howdy_internal

#include <zephyr/kernel.h>

#include "iec_fault.hpp"

namespace strucpp {

[[noreturn]] void iec_runtime_fault(IecFault reason, const char *context) noexcept
{
	ARG_UNUSED(reason);
	ARG_UNUSED(context);

	/* A board adapter must force physical outputs safe before production use. */
	k_panic();

	/* Keep the compiler's noreturn analysis valid for all Zephyr configurations. */
	for (;;) {
		k_sleep(K_FOREVER);
	}
}

} // namespace strucpp

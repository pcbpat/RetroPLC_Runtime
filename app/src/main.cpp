#ifdef RETROPLC_HAS_GENERATED_PROGRAM
#include RETROPLC_GENERATED_HEADER
#endif

#include "plc_mgmt.hpp"

#include <zephyr/dfu/mcuboot.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include <errno.h>

#ifdef RETROPLC_HAS_GENERATED_PROGRAM
namespace {

/* %QX0.0 .. %QX0.7: O1-O4 followed by LED1-LED4. */
const gpio_dt_spec digital_outputs[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(relay1), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(relay2), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(relay3), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(relay4), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led3), gpios),
};

constexpr size_t digital_output_count =
    sizeof(digital_outputs) / sizeof(digital_outputs[0]);

int initialize_digital_outputs() {
  for (const auto &output : digital_outputs) {
    if (!gpio_is_ready_dt(&output)) {
      return -ENODEV;
    }

    const int result = gpio_pin_configure_dt(&output, GPIO_OUTPUT_INACTIVE);
    if (result < 0) {
      return result;
    }
  }

  return 0;
}

int write_located_digital_outputs() {
  for (uint32_t index = 0; index < strucpp::locatedVarsCount; ++index) {
    const strucpp::LocatedVar &located = strucpp::locatedVars[index];

    if (located.area != strucpp::LocatedArea::Output ||
        located.size != strucpp::LocatedSize::Bit || located.byte_index != 0 ||
        located.bit_index >= digital_output_count ||
        located.pointer == nullptr) {
      continue;
    }

    const bool value = *static_cast<const bool *>(located.pointer);
    const int result =
        gpio_pin_set_dt(&digital_outputs[located.bit_index], value);
    if (result < 0) {
      return result;
    }
  }

  return 0;
}

} // namespace
#endif

int main() {
#ifdef RETROPLC_HAS_GENERATED_PROGRAM
  if (initialize_digital_outputs() < 0) {
    return 0;
  }

  static strucpp::RETROPLC_CONFIGURATION_CLASS configuration;
  if (configuration.get_resource_count() == 0) {
    return 0;
  }

  strucpp::ResourceInstance &resource = configuration.get_resources()[0];
  if (resource.task_count == 0 || resource.tasks[0].interval_ns <= 0) {
    return 0;
  }

  strucpp::TaskInstance &task = resource.tasks[0];
  // TODO: Add failed firmware state handling.
  boot_write_img_confirmed();

  while (true) {
    strucpp::__CURRENT_TIME_NS += task.interval_ns;

    for (size_t index = 0; index < task.program_count; ++index) {
      task.programs[index]->run();
    }

    if (write_located_digital_outputs() < 0) {
      return 0;
    }

    plc_mgmt_publish_scan(strucpp::locatedVars, strucpp::locatedVarsCount);

    k_sleep(K_NSEC(task.interval_ns));
  }
#endif

  return 0;
}

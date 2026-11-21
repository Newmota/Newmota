/********************************************************************
 * Tasmota LVGL lv_signal_bars widget
 *******************************************************************/
#include "solidify/solidified_lvgl_glob.h"

void be_load_lvgl_classes(bvm *vm) {
  be_pushntvclass(vm, &be_class_LVGL_glob);
  be_setglobal(vm, "LVGL_glob");
  be_pop(vm, 1);
}

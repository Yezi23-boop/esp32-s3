#ifndef DANGER_DETECTION_CONTROLLER_H
#define DANGER_DETECTION_CONTROLLER_H

#include "gui_guider.h"

#ifdef __cplusplus
extern "C" {
#endif

void danger_detection_controller_init(lv_ui *ui);
void danger_detection_ui_open(void);
void danger_detection_controller_poll_ui(void);

#ifdef __cplusplus
}
#endif

#endif // DANGER_DETECTION_CONTROLLER_H

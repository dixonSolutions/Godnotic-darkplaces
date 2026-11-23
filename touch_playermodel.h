// SPDX-License-Identifier: GPL-3.0-or-later
// Godnotic: uploaded player models (Settings > Player). touch_playermodel.c.
#ifndef TOUCH_PLAYERMODEL_H
#define TOUCH_PLAYERMODEL_H

struct prvm_prog_s;

void TouchPM_Init(void);
// Menu VM builtins #1223..#1225 (qcsrc/menu/xonotic/touch_uib_api.qh)
void VM_uib_pm_import(struct prvm_prog_s *prog);  // float uib_pm_import(string path)
void VM_uib_pm_report(struct prvm_prog_s *prog);  // string uib_pm_report(): "ok\t<model>\t<skin>\t<title>" or "err\t<reason>"
void VM_uib_pm_delete(struct prvm_prog_s *prog);  // float uib_pm_delete(string model)

#endif

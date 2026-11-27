/*
 * Хранилище пользовательских настроек.
 *
 * Всё лежит в том же NVS namespace "smartdash", что и адрес ELM и результаты
 * замера. Напряжение хранится в десятых долях вольта, чтобы не держать float
 * в Preferences. После загрузки и после каждого изменения вызывается
 * settings_normalize(): она восстанавливает шаги значений и порядок порогов,
 * чтобы сохранённая вручную или старой прошивкой комбинация не сломала рендер.
 */
#include "settings.h"

#include <Preferences.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

Settings gSettings;

namespace {

const char* NS = "smartdash";

// Шаги и границы настроек. Диапазоны намеренно узкие: на круглом экране
// 360x360 произвольные числа просто не поместятся в строку.
int snapInt(int v, int lo, int hi, int step) {
  if (hi < lo) hi = lo;
  if (v < lo) v = lo;
  if (v > hi) v = hi;
  int snapped = lo + ((v - lo + step / 2) / step) * step;
  return snapped > hi ? hi : snapped;
}

}  // namespace

void settings_normalize() {
  gSettings.rpmMax = snapInt(gSettings.rpmMax, 6000, 16000, 1000);
  // Между redline и максимумом должна остаться зона SETTINGS_RPM_ZONE_SPAN.
  gSettings.rpmRedline =
      snapInt(gSettings.rpmRedline, 4000, gSettings.rpmMax - SETTINGS_RPM_ZONE_SPAN, 500);

  gSettings.coolantLow = snapInt(gSettings.coolantLow, 40, 100, 5);
  gSettings.coolantHigh = snapInt(gSettings.coolantHigh, gSettings.coolantLow + 10, 140, 5);

  int lo = snapInt((int)lroundf(gSettings.voltLow * 10.0f), 90, 130, 5);
  int mid = snapInt((int)lroundf(gSettings.voltMid * 10.0f), lo + 5, 140, 5);
  int hi = snapInt((int)lroundf(gSettings.voltHigh * 10.0f), mid + 5, 180, 5);
  gSettings.voltLow = lo / 10.0f;
  gSettings.voltMid = mid / 10.0f;
  gSettings.voltHigh = hi / 10.0f;

  gSettings.bootAnim = gSettings.bootAnim ? true : false;
  gSettings.webEnabled = gSettings.webEnabled ? true : false;
  gSettings.model[SETTINGS_MODEL_LEN - 1] = '\0';
}

void settings_init() {
  gSettings.rpmMax = RPM_MAX;
  gSettings.rpmRedline = RPM_REDLINE;
  gSettings.coolantLow = 60;
  gSettings.coolantHigh = 90;
  gSettings.voltLow = 11.0f;
  gSettings.voltMid = 12.0f;
  gSettings.voltHigh = 15.0f;
  gSettings.bootAnim = true;
  // По умолчанию AP выключен: радио и HTTP не тратят ресурсы, пока они не нужны.
  gSettings.webEnabled = false;
  snprintf(gSettings.model, sizeof(gSettings.model), "%s", VEHICLE_NAME);

  Preferences p;
  p.begin(NS, false);
  gSettings.rpmMax = (int)p.getUInt("cfg_rpm_max", (uint32_t)gSettings.rpmMax);
  gSettings.rpmRedline = (int)p.getUInt("cfg_rpm_redline", (uint32_t)gSettings.rpmRedline);
  gSettings.coolantLow = (int)p.getUInt("cfg_cool_lo", (uint32_t)gSettings.coolantLow);
  gSettings.coolantHigh = (int)p.getUInt("cfg_cool_hi", (uint32_t)gSettings.coolantHigh);
  gSettings.voltLow = p.getUInt("cfg_volt_lo", (uint32_t)lroundf(gSettings.voltLow * 10.0f)) / 10.0f;
  gSettings.voltMid = p.getUInt("cfg_volt_mid", (uint32_t)lroundf(gSettings.voltMid * 10.0f)) / 10.0f;
  gSettings.voltHigh = p.getUInt("cfg_volt_hi", (uint32_t)lroundf(gSettings.voltHigh * 10.0f)) / 10.0f;
  gSettings.bootAnim = p.getUInt("cfg_boot_anim", 1) != 0;
  gSettings.webEnabled = p.getUInt("cfg_web_enabled", 0) != 0;
  String savedModel = p.getString("cfg_model", gSettings.model);
  p.end();

  snprintf(gSettings.model, sizeof(gSettings.model), "%s", savedModel.c_str());
  settings_normalize();
}

void settings_save() {
  Preferences p;
  p.begin(NS, false);
  p.putUInt("cfg_rpm_max", (uint32_t)gSettings.rpmMax);
  p.putUInt("cfg_rpm_redline", (uint32_t)gSettings.rpmRedline);
  p.putUInt("cfg_cool_lo", (uint32_t)gSettings.coolantLow);
  p.putUInt("cfg_cool_hi", (uint32_t)gSettings.coolantHigh);
  p.putUInt("cfg_volt_lo", (uint32_t)lroundf(gSettings.voltLow * 10.0f));
  p.putUInt("cfg_volt_mid", (uint32_t)lroundf(gSettings.voltMid * 10.0f));
  p.putUInt("cfg_volt_hi", (uint32_t)lroundf(gSettings.voltHigh * 10.0f));
  p.putUInt("cfg_boot_anim", gSettings.bootAnim ? 1u : 0u);
  p.putUInt("cfg_web_enabled", gSettings.webEnabled ? 1u : 0u);
  p.putString("cfg_model", gSettings.model);
  p.end();
}

# Progress Log

Completed work, findings, and session notes. Newest entries at the top.

---

## 2026-02-21 — Project Initialisation

- Created project repository with initial ESP-IDF scaffold placeholder
- Documented hardware in CLAUDE.md: ESP32-S3-WROOM-1-N16R8, 7.3" Spectra 6 EPD, TG28 PMIC,
  PCF85063 RTC, SHTC3 temp/humidity, ES7210/ES8311 audio (not used)
- Confirmed board revision has TG28 PMIC, not AXP2101
- Identified key reference firmware: aitjcize/esp32-photoframe, Waveshare demo
- Identified key risk: TG28 register compatibility with AXP2101 unverified — no public datasheet
- Created PLAN.md, DECISIONS.md, TODO.md, TEST_PLAN.md

### Open Questions
- Does TG28 respond at 0x34 and return 0x47 from register 0x03?
- Which GPIO does PCF85063 INTB connect to on the ESP32-S3? (check schematic)
- Is the Waveshare Jan 2026 commit adding TG28 support?

---

<!-- Template for future entries:

## YYYY-MM-DD — <short description>

- Bullet points of what was done
- Findings (especially hardware / register discoveries)
- Decisions made (cross-ref DECISIONS.md ADR number)
- Items moved from TODO to done

### Blocked / Issues
- ...

-->

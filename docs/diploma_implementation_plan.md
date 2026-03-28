# Diploma Implementation Plan — Edge Gateway for Digital Power Management Telemetry → MQTT

**Створено:** 2026-03-01  
**Базується на:** аналізі кодової бази `thesis_proj` (коміт `ae00ee2`) + `diploma_course_plan.md`

---

## 1 Поточний стан кодової бази (що вже є)

### 1.1 Інвентар прошивки шлюзу (`rtos_test/`)

| Компонент | Модуль | Стан |
|-----------|--------|------|
| FreeRTOS tasks (A/B/C/blinky) | `main.c` L127–147 | ✅ Повністю працює |
| PMBus I²C master (read_word, read_byte, PEC, bus recovery) | `pmbus_master.c/.h` | ✅ 7 error codes, retry loop, 9-clock recovery |
| Linear11/16 decode | `pmbus_decode.c/.h` | ✅ + host tests |
| Telemetry/status JSON encode | `telemetry.c/.h` | ✅ bounds-checked `buf_printf` |
| Events JSON encode | `events.c/.h` | ✅ 8 event types |
| Metrics (counters + gauges + p95 ring + JSON) | `metrics.c/.h` | ✅ 200-sample ring, insertion sort p95 |
| MQTT publish pipeline (single publisher) | `mqtt_gw_task.c` | ✅ `cy_mqtt_publish()` тільки з Task B |
| RAM ring buffer | `buffer_mgr.c/.h` | ✅ critical section, drop_oldest/newest |
| Flash ring buffer (Em_EEPROM, CRC, boot recovery) | `flash_buffer.c/.h` | ✅ 63 records, metadata row, auto-discard corrupted |
| Config profiles (compile-time) | `gateway_config.c/.h` + `profiles/` | ✅ 6 профілів |
| SNTP wall-clock (`wallclock.c/.h`) | `wallclock.c/.h` | ✅ `SNTP_UPDATE_DELAY=3600000` |
| `time_synced` flag in all JSON records | telemetry/status/events encoders | ✅ |
| Target simulator | `target_proj/main.c` | ✅ 11 PMBus commands, sinusoidal simulation |

### 1.2 Тести (host-side)

| Тестовий файл | Кількість тестів | Покриття |
|---------------|-----------------|----------|
| `test_buffer_ring.c` | 71 | FIFO, full, drop policy, wrap, truncation |
| `test_pmbus_decode.c` | 48 | Linear11/16, PEC CRC-8, edge cases |
| `test_json_encode.c` | 66 | Telemetry/status/event/metrics JSON + `time_synced` |
| **Разом** | **185** | |

CI відсутнє — тести запускаються вручну через `make test`.

### 1.3 Скрипти

| Скрипт | Призначення |
|--------|-------------|
| `scripts/capture/capture.py` | MQTT → JSONL (4 файли) |
| `scripts/plot/plot.py` | 5 PNG графіків з JSONL |
| `scripts/mqtt_broker/docker-compose.yml` | Mosquitto broker |

### 1.4 Документація (`docs/`)

| Файл | Опис |
|------|------|
| `architecture.md` (270 рядків) | HW/FW архітектура, task diagram, data flow, memory layout |
| `mqtt_topics.md` (201 рядок) | MQTT контракт: topics, QoS, dedup, 4 JSON schemas, SNTP timing |
| `persistent_buffer.md` (212 рядків) | Two-tier buffer, flash layout, boot recovery |
| `pmbus_command_map.md` (160 рядків) | 11 PMBus команд, wire format, decode |
| `experiments/methodology.md` (171 рядок) | Equipment, wiring, capture procedure |
| `experiments/exp1–exp4` | 4 експерименти з гіпотезами та профілями |
| `hw/bom.md`, `hw/wiring.md` | BOM та схема підключення |

---

## 2 Що ВІДСУТНЄ для дипломної (gap analysis)

| # | Можливість | Поточний стан | Потрібно для диплому |
|---|-----------|---------------|---------------------|
| 1 | Persistent `seq` | `s_seq_counter` = 0 on reboot, RAM only | A/B banks в Em_EEPROM |
| 2 | MQTT subscribe / command topic | Gateway publish-only, incoming ignored | `cmd` topic + handler |
| 3 | Runtime config change | Compile-time only (profiles) | Poll period / PEC via MQTT cmd |
| 4 | MQTT auth | `MQTT_SECURE_CONNECTION=0`, no user/pass | Username/password + ACL |
| 5 | TLS | Disabled, mbedTLS stripped | Опціонально: CA cert, TLS 1.2 |
| 6 | CI pipeline | Відсутнє | GitHub Actions: build + test |
| 7 | Multimaster handling | `CY_SCB_I2C_MASTER_ARB_LOST` → `PMBUS_ERR_BUS` (generic) | Окремі error codes + degradation FSM |
| 8 | Реальні дані з експериментів | Профілі є, скрипти є, даних немає | Прогнати Exp1–4 + нові |

---

## 3 Оцінка плану з `diploma_course_plan.md`

### 3.1 Вердикт

**План адекватний і з запасом для бакалаврської.** Виконання §3.1–3.4 + §3.5 (CI/тести) + хоча б Exp1–4 — це **дуже сильна дипломна**. Додавання §4 multimaster (Variant B) — це рівень "відмінно з відзнакою".

### 3.2 Що прибрати / спростити

| Пункт плану | Рекомендація | Причина |
|-------------|-------------|---------|
| §3.1 "двофазний commit" | **Прибрати.** Flash buffer вже має CRC + auto-discard. | Достатньо A/B banks для seq |
| §3.1 "checkpoint раз на N" для flash buffer | **Прибрати.** Вже є `total_writes` + meta row CRC. | Вже реалізовано |
| §3.2 "персистенція профілю у NVM (A/B + CRC)" | **Відкласти.** Runtime зміна через MQTT достатня. | Overengineering для баклавра |
| §4 Variant A (повний multimaster) | **Не робити.** Це магістерський рівень. | Variant B (detect + degrade) достатньо |
| §3.3 "N=8–16 devices" | **Обмежити N=4.** 4 target'и ще реалістично для стенду. | Немає стільки плат |

### 3.3 Що критично додати

| Блок | Чому обов'язковий | Оцінка трудомісткості |
|------|-------------------|----------------------|
| CI pipeline (GitHub Actions) | Показує інженерну зрілість, тривіально | 0.5 дня |
| Persistent `seq` (A/B Em_EEPROM) | "Що після ребуту?" — перше питання комісії | 2 дні |
| MQTT auth (user/pass + ACL) | "А де безпека?" | 1 день |
| MQTT command topic (poll period + PEC toggle) | Ключова відмінність від курсової — *керований* gateway | 4–5 днів |
| Реальні експерименти з даними | Без даних немає результатів у дипломній | 3–4 дні |

---

## 4 Покроковий план реалізації

### Фаза 1: CI + тестова інфраструктура (0.5 дня)

**Ціль:** автоматична збірка + тести на кожен push.

**Створити:**
- `.github/workflows/ci.yml`:
  - Job 1: `make test` (host unit tests, Ubuntu runner)
  - Job 2: `make build` з `arm-none-eabi-gcc` (gateway firmware, matrix по 3 профілях: `default`, `exp1_fast`, `exp3_offline`)
- Badge у кореневий `README.md`

**Критерій готовності:** зелений badge на GitHub після push.

---

### Фаза 2: Persistent `seq` (2 дні)

**Ціль:** `seq` переживає reboot → cross-reboot dedup працює.

**Архітектура:** A/B ping-pong banks у Em_EEPROM (rows 62–63, зменшити data rows до 61).

Кожен bank (512 байт, використовується 16):
```
offset  field           size
0       magic           4 B   (0x53455100)
4       seq_value       4 B
8       boot_count      4 B
12      crc32           4 B
```

**Створити:**
- `source/persistent_seq.h` — API: `init()`, `checkpoint(uint32_t seq)`, `get_boot_count()`
- `source/persistent_seq.c` — A/B read, CRC validate, pick higher seq, ping-pong write
- `tests/test_persistent_seq.c` — A/B recovery, single corruption fallback, both corrupt → seq=0

**Змінити:**
- `gateway_ipc.c` — init відновлює `s_seq_counter`, checkpoint кожні 100 seq або кожні 5 с
- `flash_buffer.h` — `FLASH_BUF_DATA_ROWS` 63 → 61
- `docs/persistent_buffer.md` — додати layout persistent seq
- `docs/mqtt_topics.md` §3 — "seq persists across reboots (A/B Em_EEPROM)"

**Критерій готовності:** тест `test_persistent_seq` pass + gateway reboot → seq продовжує з останнього checkpoint.

---

### Фаза 3: MQTT authentication + ACL (1 день)

**Ціль:** MQTT з username/password, broker обмежує топіки.

**Змінити:**
- `gateway_config.h` — додати `mqtt.username[32]`, `mqtt.password[64]`
- `gateway_config.c` / профілі — заповнити credentials
- `mqtt_gw_task.c` — передати credentials в `cy_mqtt_connect()` → `broker_info`
- `mqtt_client_config.h` — задокументувати auth поля
- `scripts/mqtt_broker/docker-compose.yml` — volume для password file + ACL

**Створити:**
- `scripts/mqtt_broker/mosquitto_passwords.txt` — `gateway:hashed_password`
- `scripts/mqtt_broker/mosquitto_acl.conf`:
  ```
  user gateway
  topic write pmbus/#
  topic read  pmbus/+/cmd
  ```

**Змінити:** `docs/architecture.md` — Security section.

**Критерій готовності:** gateway підключається з auth; без auth — rejected.

---

### Фаза 4: MQTT command topic — runtime конфігурація (4–5 днів)

**Ціль:** змінювати poll period / PEC / status period через MQTT.

**Топік:** `pmbus/{gw_id}/cmd` (subscribe, QoS 1)  
**Відповідь:** `pmbus/{gw_id}/config` (publish)

**Команди (JSON):**
```json
{ "cmd": "set_poll_period", "addr": "0x58", "value_ms": 500 }
{ "cmd": "set_pec", "value": false }
{ "cmd": "set_status_period", "addr": "0x58", "value_ms": 30000 }
{ "cmd": "get_config" }
{ "cmd": "reboot" }
```

**Створити:**
- `source/cmd_handler.h` — enum `cmd_type_t`, API: `init()`, `process(payload, len)`
- `source/cmd_handler.c`:
  - Парсинг JSON (мінімальний — `sscanf` або tiny JSON parser)
  - Whitelist команд (enum switch)
  - Rate-limit: 1 cmd/sec (лічильник + timestamp)
  - Валідація: poll_period 50–60000 ms, addr в `g_config.devices[]`
- `tests/test_cmd_handler.c` — парсинг, whitelist rejection, rate-limit, boundary values

**Змінити:**
- `gateway_config.h` — додати `config_set_poll_period()`, `config_set_pec()` з atomic access
- `mqtt_gw_task.c`:
  - Subscribe на `pmbus/{gw_id}/cmd` після connect
  - Event callback → `cmd_handler_process()`
  - `get_config` → publish JSON dump на `.../config`
- `pmbus_poll_task.c` — читати `poll_period_ms` через getter (для підхоплення runtime змін)
- `events.h/.c` — нові типи: `CMD_RECEIVED`, `CMD_REJECTED`
- `docs/mqtt_topics.md` — §1.5 Command topic, §4.5 Command/Config payload schemas
- `docs/architecture.md` — command data flow

**Критерій готовності:** `mosquitto_pub -t pmbus/gw01/cmd -m '{"cmd":"set_poll_period","addr":"0x58","value_ms":200}'` → gateway підхоплює нову частоту + emits event.

---

### Фаза 5: Multimaster — Variant B: graceful degradation (3–4 дні)

> **Опціонально.** Робити тільки якщо є час і доступ до другого master-пристрою.

**Ціль:** gateway детектує конкуренцію на шині і безпечно деградує.

**Змінити:**
- `pmbus_master.h` — нові error codes: `PMBUS_ERR_ARB_LOST`, `PMBUS_ERR_BUS_BUSY`
- `pmbus_master.c`:
  - Перед START: check bus idle (GPIO read SCL+SDA)
  - Розділити `CY_SCB_I2C_MASTER_ARB_LOST` → `PMBUS_ERR_ARB_LOST`
  - Exponential backoff з jitter (5ms base, 500ms max) при ARB_LOST
- `metrics.h/.c` — нові counters: `i2c_arb_lost`, `i2c_bus_busy`; gauge: `bus_health` (HEALTHY/DEGRADED)
- `pmbus_poll_task.c` — degradation FSM:
  - `arb_lost > 5` за window → DEGRADED: poll ×4, event `BUS_MULTIMASTER_DETECTED`
  - `arb_lost < 2` за 3 windows → HEALTHY: restore, event `BUS_HEALTH_RESTORED`
- `events.h/.c` — `BUS_MULTIMASTER_DETECTED`, `BUS_HEALTH_RESTORED`, `BUS_HEALTH_DEGRADED`
- `docs/mqtt_topics.md` — нові counters/gauges в metrics schema

**Створити:**
- `docs/multimaster.md` — Variant B design, FSM diagram, метрики
- Тести: mock I2C arb_lost → verify backoff timing + FSM transitions

**Критерій готовності:** другий master на шині → gateway log показує degraded mode → після зняття другого master → recovery.

---

### Фаза 6: Експерименти + збір даних (3–4 дні)

**Ціль:** реальні дані для 4–6 експериментів.

| Експеримент | Профіль | Тривалість | Що вимірюється |
|-------------|---------|------------|----------------|
| Exp1: Latency | `exp1_fast`, `exp1_single` | 5 хв × 3 runs | p95 read-to-publish при різних poll periods |
| Exp2: Throughput | `exp2_throughput` | 5 хв | max msg/s, queue drops, error rate |
| Exp3: Offline buffer | `exp3_offline` | 10 хв | Wi-Fi kill → buffer fill → reconnect → flush time |
| Exp4: PEC on/off | `exp4_pec_off` + default | 5 хв × 2 | latency delta, PEC fail count |
| Exp5: Runtime config | default → cmd | 5 хв | Зміна poll period mid-capture, видно на графіку |
| Exp6: Multimaster* | custom | 5 хв | Degradation/recovery при другому master |

\* Exp6 тільки якщо реалізована Фаза 5.

**Кроки:**
1. Прогнати кожен експеримент за `docs/experiments/execution_guide.md`
2. Зберегти JSONL у `scripts/logs/<experiment>/`
3. Згенерувати графіки через `plot.py`
4. Зберегти PNG у `docs/experiments/results/`

**Критерій готовності:** мінімум 4 набори графіків (latency.png, buffer.png, errors.png, throughput.png) з реальними даними.

---

## 5 Черга пріоритетів (рекомендований порядок виконання)

```
Фаза 1 (CI)              ████  0.5 дня    ← зробити одразу
Фаза 2 (persistent seq)  ████████  2 дні
Фаза 3 (MQTT auth)       ████  1 день     ← легка перемога
Фаза 4 (MQTT cmd)        ████████████  4–5 днів  ← основна фіча
Фаза 5 (multimaster B)   ██████████  3–4 дні     ← бонус для "відмінно"
Фаза 6 (експерименти)    ████████  3–4 дні       ← завжди останнє
                          ──────────────────────
                          ~14–18 робочих днів
```

**Мінімум для "пристойної дипломної":** Фази 1 + 2 + 3 + 4 + 6 (~11 днів).  
**Для "відмінно":** + Фаза 5 (~14–18 днів).

---

## 6 Файли, які будуть створені/змінені

### Нові файли (8–10)

| Файл | Фаза |
|------|------|
| `.github/workflows/ci.yml` | 1 |
| `source/persistent_seq.h` | 2 |
| `source/persistent_seq.c` | 2 |
| `tests/test_persistent_seq.c` | 2 |
| `scripts/mqtt_broker/mosquitto_acl.conf` | 3 |
| `scripts/mqtt_broker/mosquitto_passwords.txt` | 3 |
| `source/cmd_handler.h` | 4 |
| `source/cmd_handler.c` | 4 |
| `tests/test_cmd_handler.c` | 4 |
| `docs/multimaster.md` | 5 (опціонально) |
| `docs/smbus_timeout_recovery.md` | Future work (опціонально) |

### Файли, що зміняться (по фазах)

| Файл | Фази |
|------|------|
| `README.md` (root) | 1 |
| `gateway_ipc.c` | 2 |
| `flash_buffer.h` | 2 |
| `docs/persistent_buffer.md` | 2 |
| `docs/mqtt_topics.md` | 2, 4 |
| `gateway_config.h` | 3, 4 |
| `gateway_config.c` + профілі | 3, 4 |
| `mqtt_gw_task.c` | 3, 4 |
| `mqtt_client_config.h` | 3 |
| `docker-compose.yml` | 3 |
| `docs/architecture.md` | 3, 4, 5 |
| `pmbus_poll_task.c` | 4, 5 |
| `events.h/.c` | 4, 5 |
| `pmbus_master.c/.h` | 5 |
| `metrics.c/.h` | 5 |

---

## 7 Ризики та мітигація

| Ризик | Ймовірність | Мітигація |
|-------|-------------|-----------|
| `arm-none-eabi-gcc` в GitHub Actions не збирає MTB проєкт | Висока | Використати Docker image з MTB tools або обмежити CI лише host tests |
| Em_EEPROM rows 62–63 вже використовуються чимось | Низька | Перевірити linker script; Em_EEPROM = окремий регіон |
| Другий master для Exp6 недоступний | Середня | Пропустити Фазу 5, описати як "future work" |
| JSON парсер для cmd handler занадто великий для MCU | Середня | Використати мінімальний `sscanf`-based парсер або cJSON (~8 KB flash) |
| TLS збільшує flash на ~60–80 KB | Низька | Залишити TLS опціональним (окремий профіль), auth без TLS достатньо |

---

## 8 Відкладене future work (поза scope курсової)

1. **SMBus SCL-low timeout recovery через hardware timer.**
   Ідея полягає в тому, щоб не опиратися лише на software `transaction_timeout_ms` та
   `9xSCL` recovery, а мати окремий hardware monitor лінії `SCL`. Таймер має
   відліковувати timeout лише поки `SCL` утримується low, а після спрацювання
   ISR має abort-ити локальну транзакцію, відновлювати SCB state і дозволяти
   перезапуск обміну після recovery gap. Додатково цей самий механізм можна
   використати як active bus reset, утримуючи `SCL` low достатньо довго для
   timeout-driven reset усіх PMBus/SMBus пристроїв на шині. Для курсової це
   вважається overkill; повертатися до цієї ідеї має сенс уже в бакалаврській,
   якщо hot-plug/recovery robustness стане окремою ціллю. Деталі:
   `docs/smbus_timeout_recovery.md`.

2. **Idle-bus timeout cooldown after successful SCB reset.**
   Current PMBus hot-plug logs show that the hard lock is fixed and bus-low
   hammering is reduced, but repeated `TIMEOUT` still happens on an apparently
   idle bus (`scl=1`, `sda=1`) after `timeout-still-busy ->
   controller-reset-after`. The follow-up task is to stop retrying the same
   PMBus command after a successful idle-bus controller reset and/or arm a
   short per-device cooldown before the next poll cycle. Goal: shorten the
   remaining degraded tail after hot-plug events without reintroducing the
   previous stuck-controller state.

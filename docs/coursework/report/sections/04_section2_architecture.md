# 2. РОЗДІЛ 2. РОЗРОБЛЕННЯ АРХІТЕКТУРИ ПРОГРАМНОГО ЗАБЕЗПЕЧЕННЯ

## 2.1. Загальна концепція та модульна структура ПЗ

Архітектура програмного забезпечення gateway побудована на базі FreeRTOS, що
дозволяє розділити незалежні функції системи на окремі задачі з фіксованими
пріоритетами та передбачуваною часовою поведінкою [8], [9]. Такий підхід є
критичним для системи, у якій одночасно виконуються:

1. циклічне опитування PMBus-пристроїв;
2. мережеве підключення та MQTT-публікація;
3. буферизація даних при втраті каналу зв'язку.

Логічно програмне забезпечення розділено на такі рівні:

1. Рівень апаратних абстракцій (BSP/HAL/PDL):
   ініціалізація платформи, периферії, I2C-блоку, мережевого стека [10], [11].
2. Рівень драйверів:
   PMBus master (`pmbus_master.*`) з підтримкою retry/timeout/PEC [2], [4], [5].
3. Рівень RTOS-задач:
   `pmbus_poll_task`, `mqtt_gw_task`, `buffer_task`.
4. Рівень прикладних сервісів:
   декодування PMBus, формування JSON, облік метрик, події, буферизація,
   контракти MQTT.

Окремо виділено дві прошивки:

1. `rtos_test` (gateway) - збір, обробка і передача телеметрії;
2. `target_proj` (target) - PMBus slave-модель джерела живлення.

Така ізоляція підвищує керованість розробки: мережеву логіку можна змінювати
незалежно від моделі target, а PMBus-поведінку тестувати окремо від
MQTT-транспорту.

## 2.2. Багатозадачна модель та міжпроцесна взаємодія (IPC)

У runtime-системі gateway використовуються три основні задачі:

1. `pmbus_poll_task` (пріоритет 4):
   ініціалізує PMBus master, виконує періодичне опитування команд телеметрії та
   статусу, формує записи і передає їх у IPC-черги.
2. `mqtt_gw_task` (пріоритет 3):
   керує Wi-Fi/MQTT з'єднанням, реалізує reconnect з exponential backoff,
   читає IPC-черги, публікує JSON-повідомлення, а також запускає flush
   буферизованих записів.
3. `buffer_task` (пріоритет 2):
   виконує фоновий супровід буфера (оновлення gauge-метрик глибини RAM/Flash);
   безпосередню MQTT-публікацію не виконує.

Основний IPC реалізовано в модулі `gateway_ipc` через три черги:

1. `telemetry_queue` - для telemetry-records;
2. `status_queue` - для status-records;
3. `event_queue` - для event-records.

Додатково в `gateway_ipc` підтримуються:

1. глобальний `seq`-лічильник;
2. прапор стану `mqtt_online`;
3. функція формування часової мітки `now_ms`.

Ця модель забезпечує слабке зв'язування між задачами: producer (poll task) не
залежить від мережевого стану, а transport-логіка (MQTT task) працює зі
стандартизованими структурами даних.

Параметри задач у поточній реалізації:

1. `pmbus_poll_task`: stack `1024`, priority `4`;
2. `mqtt_gw_task`: stack `3072`, priority `3`;
3. `buffer_task`: stack `1024`, priority `2`.

Фрагмент коду 2.1 - створення задач у `main.c`:

```c
/* Task A — PMBus polling */
xTaskCreate(pmbus_poll_task, "PMBus_Poll",
            PMBUS_POLL_TASK_STACK_SIZE, NULL,
            PMBUS_POLL_TASK_PRIORITY, NULL);

/* Task B — MQTT gateway */
xTaskCreate(mqtt_gw_task, "MQTT_GW",
            MQTT_GW_TASK_STACK_SIZE, NULL,
            MQTT_GW_TASK_PRIORITY, NULL);

/* Task C — Buffer task */
xTaskCreate(buffer_task, "Buffer",
            BUFFER_TASK_STACK_SIZE, NULL,
            BUFFER_TASK_PRIORITY, NULL);
```

Фрагмент коду 2.2 - ініціалізація IPC-черг у `gateway_ipc.c`:

```c
s_telemetry_q = xQueueCreate(IPC_TELEMETRY_QUEUE_DEPTH,
                             sizeof(telemetry_record_t));
s_status_q    = xQueueCreate(IPC_STATUS_QUEUE_DEPTH,
                             sizeof(status_record_t));
s_event_q     = xQueueCreate(IPC_EVENT_QUEUE_DEPTH,
                             sizeof(event_record_t));

if (s_telemetry_q == NULL || s_status_q == NULL || s_event_q == NULL)
{
    printf("[IPC] ERROR: Failed to create queues\n");
    return false;
}
```

З урахуванням фіксованих глибин черг (`64/16/16`) ця схема забезпечує
передбачуване використання RAM і контрольовану деградацію при піковому
навантаженні.

[[ВСТАВИТИ РИСУНОК 2.1 ТУТ]]
Підпис: Рисунок 2.1 - Блок-схема взаємодії задач FreeRTOS та IPC-черг у gateway.
Файл: `docs/coursework/diagrams/exports/uml_component.png`.

## 2.3. Модель даних та MQTT-контракти

Потік даних у gateway організовано як послідовне перетворення:

1. PMBus-транзакції повертають сирі байти регістрів;
2. модуль декодування переводить значення у інженерні одиниці;
3. формується типізована структура (telemetry/status/event);
4. виконується JSON-кодування та MQTT-публікація.

Для передачі даних застосовано модель single-publisher: усі `publish` операції
серіалізовані в `mqtt_gw_task`. Це спрощує контроль QoS, повторних спроб і
відновлення після outage [6], [7].

Фрагмент коду 2.3 - шлях telemetry-record у `mqtt_gw_task.c`:

```c
while (xQueueReceive(gateway_ipc_telemetry_queue(), &rec, 0) == pdTRUE)
{
    int json_len = encode_telemetry_json(&rec, s_json_buf, JSON_BUF_SIZE);
    if (json_len <= 0) { continue; }

    int topic_len = build_device_topic(s_topic_buf, TOPIC_BUF_SIZE,
                                       rec.addr_7bit, "telemetry");
    if (topic_len <= 0) { continue; }

    if (!publish_json(s_topic_buf, s_json_buf, (size_t)json_len))
    {
        buffer_mgr_put(s_topic_buf, s_json_buf, (uint16_t)json_len);
    }
}
```

Наведений фрагмент демонструє ключовий архітектурний інваріант: дані не
втрачаються при невдалій публікації, а переводяться в підсистему буферизації.

Логічно потоки розділяються на:

1. телеметрію (напруга, струм, температура, потужність);
2. статусні регістри (`STATUS_*`);
3. події системи;
4. метрики продуктивності (counters/rates/latency).

Розподіл за топіками виконується відповідно до внутрішніх контрактів проєкту;
формат повідомлень узгоджується з вимогами MQTT/JSON [6], [7].

## 2.4. Підсистема буферизації (Store-and-Forward)

Для забезпечення відмовостійкості використовується дворівнева модель
store-and-forward:

1. оперативний RAM ring buffer (швидкий рівень);
2. опційний Flash/Em_EEPROM рівень (персистентний рівень).

Алгоритм обробки в аварійному та відновлювальному режимах:

1. при недоступності MQTT нові записи зберігаються у буфер (`buffer_mgr_put`);
2. при заповненні RAM (і дозволеному flash-рівні) виконується spill у flash;
3. після відновлення зв'язку `mqtt_gw_task` виконує flush за схемою
   `peek -> publish -> consume`;
4. порядок вивантаження: спочатку flash FIFO (старіші записи), потім RAM FIFO.

Така організація дозволяє не зупиняти цикл опитування PMBus під час outage та
забезпечувати контрольоване відновлення доставки після reconnect.

Фрагмент коду 2.4 - flush буфера у `mqtt_gw_task.c`:

```c
/* Phase 1: flash first */
while (flushed < g_config.buffer.flush_batch_size &&
       flash_buffer_peek(&rec))
{
    if (!publish_json(rec.topic, rec.payload, rec.payload_len)) { break; }
    flash_buffer_consume();
    flushed++;
}

/* Phase 2: RAM second */
while (flushed < g_config.buffer.flush_batch_size &&
       buffer_mgr_peek(&rec))
{
    if (!publish_json(rec.topic, rec.payload, rec.payload_len)) { break; }
    buffer_mgr_consume();
    flushed++;
}
```

Фрагмент коду 2.5 - reconnect/backoff у `mqtt_gw_task.c`:

```c
static void backoff_wait(void)
{
    vTaskDelay(pdMS_TO_TICKS(s_backoff_ms));
    s_backoff_ms *= 2u;
    if (s_backoff_ms > g_config.mqtt.backoff_max_ms)
    {
        s_backoff_ms = g_config.mqtt.backoff_max_ms;
    }
}
```

Таким чином, у recovery-сценарії система поєднує два механізми:

1. стабілізацію підключення через exponential backoff;
2. гарантоване FIFO-вивантаження накопичених повідомлень.

## 2.5. UML-моделювання архітектури ПЗ

Для графічного подання архітектури використовуються дві UML-діаграми:

1. UML component / блок-схема задач і модулів:
   відображає статичну структуру підсистем gateway, зв'язки між задачами та
   IPC-каналами.
2. UML sequence:
   показує динаміку обміну повідомленнями в сценарії
   `PMBus read -> queue -> JSON -> MQTT` та у сценарії
   `outage -> buffer -> recovery -> flush`.

[[ВСТАВИТИ РИСУНОК 2.2 ТУТ]]
Підпис: Рисунок 2.2 - UML Sequence Diagram для сценарію store-and-forward.
Файл: `docs/coursework/diagrams/exports/uml_sequence_pmbus_to_mqtt.png`.

Примітка: діаграма component (рис. 2.1) та sequence (рис. 2.2) повинні бути
також винесені до додатків у форматах PNG/PDF.

## 2.6. Висновки до розділу 2

У Розділі 2 сформовано архітектуру програмного забезпечення gateway на базі
FreeRTOS із чітким поділом відповідальностей між задачами опитування, мережевої
передачі та буферного супроводу. Показано, що така декомпозиція забезпечує:

1. детерміноване виконання критичних циклів опитування;
2. централізований контроль мережевої публікації;
3. відмовостійку доставку телеметрії через механізм store-and-forward.

Запропонована модель IPC і буферизації відповідає задачі надійного збору та
доставки даних у системах моніторингу живлення. Це створює завершену основу
для подальшого опису реалізації модулів у Розділі 3 та методики перевірки у
Розділі 4.

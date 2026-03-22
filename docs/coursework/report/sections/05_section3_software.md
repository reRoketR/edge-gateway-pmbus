# 3. РОЗДІЛ 3. РОЗРОБЛЕННЯ ПРОГРАМНОГО ЗАБЕЗПЕЧЕННЯ МІКРОКОМП'ЮТЕРА МПС

## 3.1. Середовище розробки та інструментальні засоби

Розроблення ПЗ мікрокомп'ютера gateway виконано на стеку Infineon
ModusToolbox для платформи PSoC 6. Такий вибір забезпечує узгоджене
поєднання:

1. засобів побудови прошивки (make-based build system);
2. пакетів підтримки апаратної платформи (BSP/HAL/PDL);
3. RTOS і мережевих компонентів для IoT-шлюзу.

Практичне середовище щоденної розробки у межах проєкту - `Visual Studio Code`
з розширенням `ModusToolbox Assistant`. Така конфігурація дає єдину робочу
точку для редагування коду, запуску команд складання, прошивання плати,
перегляду журналів виконання та швидкого переходу між модулями.

З інженерної точки зору це дозволяє поєднати переваги "легкого" редактора
коду з офіційним інструментарієм виробника платформи: низький час старту
сесії розробки, прозорий виклик `make`-цілей і зручну трасованість змін між
вихідними файлами, конфігурацією та результатами збірки.

[[ВСТАВИТИ РИСУНОК 3.1 ТУТ]]
Підпис: Рисунок 3.1 - Інтерфейс середовища розробки `Visual Studio Code` з
розширенням `ModusToolbox Assistant` під час складання, запуску та
відлагодження програмних модулів gateway.
Файл: `docs/coursework/figures/vscode_modustoolbox_debug.png`.

У проєкті використано цільову плату `CY8CKIT-062S2-43012`, для якої в системі
збирання явно задано відповідний BSP target. Компіляція виконується
інструментальним ланцюжком `GCC_ARM`, що відповідає типовому workflow
ModusToolbox.

Фрагмент коду 3.1 - базова конфігурація інструментарію (`Makefile`):

```make
TARGET=APP_CY8CKIT-062S2-43012
APPNAME=pmbus-mqtt-gateway
TOOLCHAIN=GCC_ARM
CONFIG=Debug

COMPONENTS=FREERTOS LWIP MBEDTLS SECURE_SOCKETS
```

Наведені параметри визначають:

1. апаратну ціль і build-профіль;
2. ядро RTOS (`FREERTOS`) для багатозадачної архітектури;
3. мережевий стек `LWIP` і криптографічну підсистему `MBEDTLS`;
4. рівень secure sockets для транспортної інтеграції MQTT-клієнта.

З погляду програмної архітектури (див. Розділ 2) це середовище безпосередньо
підтримує необхідні шари реалізації:

1. `BSP/HAL/PDL` - апаратна ініціалізація і периферія (UART, I2C, GPIO) [10];
2. `FreeRTOS API` - задачі, черги, синхронізація [8], [9];
3. `lwIP` - мережеві сервіси часу (SNTP) [14];
4. `Infineon MQTT library` - підключення, publish/subscribe, callback-модель [11].

Для відтворюваності експериментів у `Makefile` також зафіксовано профільний
механізм конфігурації (`GW_PROFILE`), що дозволяє перемикати режими роботи
gateway без зміни коду модулів:

```make
ifneq ($(GW_PROFILE),)
DEFINES+= GW_PROFILE_HEADER='"profiles/profile_$(GW_PROFILE).h"'
endif
```

Це важливо для курсового проєкту, оскільки різні сценарії (штатний режим,
стрес-тест, offline/recovery) можуть запускатися на однаковій кодовій базі
лише через зміну профільного заголовка.

Практичні команди збирання і запуску тестів:

```bash
make build TOOLCHAIN=GCC_ARM CONFIG=Debug
make program
make test
```

У практичному циклі розробки ці команди застосовуються ітеративно:

1. локальне внесення змін у модуль;
2. збірка прошивки в `Debug`-конфігурації;
3. прошивання плати та валідація поведінки на стенді;
4. запуск host-side тестів для перевірки, що зміни не внесли регресій.

Такий цикл є важливим саме для вбудованого ПЗ, оскільки дає можливість
оперативно зіставляти зміни коду з фактичною поведінкою системи на апаратурі
та одночасно утримувати контроль над якістю алгоритмічної частини.

Підхід до тестування комбінований:

1. target-side збірка і запуск прошивки на стенді;
2. host-side unit-тести для критичних модулів (`pmbus_decode`,
   JSON-кодування, buffer logic), що зменшує ризик регресій ще до
   апаратної перевірки.

Отже, обране середовище розробки забезпечує повний інструментальний цикл:
від конфігурації BSP і RTOS до мережевої взаємодії та автоматизованої перевірки
частини функціональності. Це створює технологічну основу для підрозділу 3.2,
де буде розглянуто послідовність ініціалізації мікрокомп'ютера від `main()`.

## 3.2. Модуль ініціалізації мікрокомп'ютера

Модуль ініціалізації реалізовано у функції `main()` і він визначає "керований
вхід" системи в робочий режим: від старту апаратної платформи до передавання
керування планувальнику FreeRTOS. Для gateway це критично, оскільки помилка на
етапі boot має бути виявлена одразу (fail-fast), а не вже під навантаженням
під час опитування PMBus або публікації MQTT.

У поточній реалізації послідовність запуску має такий вигляд:

1. обробка secure-специфіки (скидання WDT, якщо платформа з secure boot);
2. ініціалізація BSP і глобального переривального контексту;
3. запуск debug UART (`retarget-io`) і виведення boot-банера;
4. ініціалізація системних підсистем (`metrics`, `gateway_ipc`, `buffer_mgr`);
5. створення RTOS-задач (`pmbus_poll_task`, `mqtt_gw_task`, `buffer_task`,
   `blinky_task`);
6. запуск `vTaskStartScheduler()` з переходом у багатозадачний режим [8], [9].

Допоміжна задача `blinky_task` використовується як heartbeat-індикатор
життєздатності системи: періодичне перемикання користувацького світлодіода
підтверджує факт успішного запуску планувальника та базову працездатність
RTOS-середовища.

Фрагмент коду 3.2 - апаратна ініціалізація та запуск UART-каналу (`main.c`):

```c
/* Initialize the board support package. */
result = cybsp_init();
CY_ASSERT(CY_RSLT_SUCCESS == result);

/* Enable global interrupts. */
__enable_irq();

/* Initialize retarget-io to use the debug UART port. */
result = cy_retarget_io_init(CYBSP_DEBUG_UART_TX, CYBSP_DEBUG_UART_RX,
                             CY_RETARGET_IO_BAUDRATE);
CY_ASSERT(CY_RSLT_SUCCESS == result);
```

Цей етап формує мінімальне надійне середовище виконання: ініціалізована
платформа, доступний канал журналювання і дозволені переривання. Наявність
UART-логування на старті спрощує діагностику ранніх відмов, коли RTOS-задачі
ще не запущені.

Після апаратного bootstrap система переходить до ініціалізації внутрішніх
підсистем із політикою "завершити запуск тільки за повної готовності".

Фрагмент коду 3.3 - fail-fast ініціалізація IPC та буфера (`main.c`):

```c
metrics_init();
printf("[SYS] Metrics initialised\n");

if (!gateway_ipc_init())
{
    printf("[SYS] FATAL: IPC init failed\n");
    CY_ASSERT(0);
    for (;;) { __WFI(); }
}

if (!buffer_mgr_init())
{
    printf("[SYS] FATAL: Buffer manager init failed\n");
    CY_ASSERT(0);
    for (;;) { __WFI(); }
}
```

Така логіка гарантує, що система не перейде в активну фазу збору/передачі
даних без працездатного міжзадачного обміну та механізму store-and-forward.
Для вбудованого шлюзу це принципово: частково ініціалізована система є
небезпечнішою за керовану зупинку на етапі boot.

Після успішної підготовки інфраструктури створюються задачі і запускається
планувальник.

Фрагмент коду 3.4 - створення задач і запуск планувальника (`main.c`):

```c
xTaskCreate(pmbus_poll_task, "PMBus_Poll",
            PMBUS_POLL_TASK_STACK_SIZE, NULL,
            PMBUS_POLL_TASK_PRIORITY, NULL);

xTaskCreate(mqtt_gw_task, "MQTT_GW",
            MQTT_GW_TASK_STACK_SIZE, NULL,
            MQTT_GW_TASK_PRIORITY, NULL);

xTaskCreate(buffer_task, "Buffer",
            BUFFER_TASK_STACK_SIZE, NULL,
            BUFFER_TASK_PRIORITY, NULL);

vTaskStartScheduler();
```

Використаний порядок і пріоритезація задач відповідають архітектурному рішенню
Розділу 2: опитування PMBus має вищий пріоритет за мережеву задачу, а буферний
супровід працює фоново [8], [9].

Окремо важливо зафіксувати, що частина ініціалізації виконується не в `main()`,
а в тілах задач після старту планувальника:

1. `pmbus_poll_task` викликає `pmbus_init()` і піднімає I2C/PMBus контур [10];
2. `mqtt_gw_task` виконує `cy_wcm_init()`, ініціалізацію MQTT-бібліотеки та
   встановлення сесії з брокером [11].

Це зменшує час блокування в `main()`, спрощує локалізацію помилок за
підсистемами і робить старт системи більш масштабованим для подальшого
розширення складу задач.

[[ВСТАВИТИ РИСУНОК 3.2 ТУТ]]
Підпис: Рисунок 3.2 - Алгоритм ініціалізації gateway від `main()` до старту
планувальника FreeRTOS.
Файл: `docs/coursework/diagrams/exports/flow_init_main_to_scheduler.png`.

## 3.3. Алгоритм роботи драйвера та задачі опитування PMBus

Підсистема опитування PMBus у проєкті складається з трьох взаємопов'язаних
модулів:

1. `pmbus_master` - низькорівневий драйвер I2C/SMBus (ініціалізація, транзакції,
   retry/timeout, PEC, bus-recovery);
2. `pmbus_poll_task` - RTOS-задача періодичного опитування пристроїв і
   підготовки телеметричних/статусних записів;
3. `pmbus_decode` - чисті функції декодування форматів Linear11/Linear16 у
   фізичні величини в milli-одиницях [2], [4], [5].

Такий поділ дозволяє відокремити транспортний рівень (I2C/SMBus транзакції) від
алгоритмів опитування та від математичного перетворення даних, що знижує
зв'язність і підвищує тестованість коду.

Алгоритм старту PMBus-контуру починається з `pmbus_init()`, який конфігурує SCB
блок мікроконтролера, налаштовує частоту шини, підключає ISR та переводить
драйвер у стан "готовий до транзакцій" [10].

Фрагмент коду 3.5 - ініціалізація PMBus master (`pmbus_master.c`):

```c
status = Cy_SCB_I2C_Init(PMBUS_CONTROLLER_HW, &PMBUS_CONTROLLER_config,
                         &pmbus_i2c_ctx);
if (CY_SCB_I2C_SUCCESS != status) { return PMBUS_ERR_BUS; }

uint32_t actual_rate = Cy_SCB_I2C_SetDataRate(PMBUS_CONTROLLER_HW,
                                              g_config.i2c.speed_hz,
                                              actual_scb_clk);

(void)Cy_SysInt_Init(&pmbus_scb3_irq_cfg, pmbus_scb3_isr);
NVIC_EnableIRQ(pmbus_scb3_irq_cfg.intrSrc);
Cy_SCB_I2C_Enable(PMBUS_CONTROLLER_HW);
```

У штатному профілі система опитує два PMBus-пристрої за адресами `0x58` і
`0x59`, а параметри шини беруться з конфігурації профілю (`speed_hz`,
`timeout_ms`, `retries`, `pec_enabled`).

Базова операція драйвера - SMBus Read Word. Вона виконується у два етапи
(`write cmd` + repeated-start `read data`) з тайм-аутом, а за увімкненого PEC -
з перевіркою CRC-8 (поліном `0x07`) [2], [4], [5].

Фрагмент коду 3.6 - транзакція `pmbus_read_word()` з перевіркою PEC
(`pmbus_master.c`):

```c
/* Phase 1: write command without STOP */
pdl_st = Cy_SCB_I2C_MasterWrite(PMBUS_CONTROLLER_HW, &wr_cfg, &pmbus_i2c_ctx);
result = wait_for_completion(pmbus_timeout_ms);

/* Phase 2: read 2 bytes (+PEC if enabled) */
pdl_st = Cy_SCB_I2C_MasterRead(PMBUS_CONTROLLER_HW, &rd_cfg, &pmbus_i2c_ctx);
result = wait_for_completion(pmbus_timeout_ms);

if (pec)
{
    uint8_t pec_input[5] = { (addr_7bit << 1) | 0u, cmd,
                             (addr_7bit << 1) | 1u, rd_buf[0], rd_buf[1] };
    if (pmbus_crc8(pec_input, 5u) != rd_buf[2]) { result = PMBUS_ERR_PEC; }
}
```

На рівні задачі `pmbus_poll_task` реалізовано детермінований цикл опитування:
періодичне пробудження через `vTaskDelayUntil` з базовим кроком 10 мс, окремі
дедлайни для telemetry/status на кожен пристрій, а також backoff при
послідовних помилках (офлайн-поведінка) [8], [9].

Ключові особливості алгоритму:

1. кешування `VOUT_MODE` (експонента Linear16) з періодичним retry;
2. послідовне читання команд `READ_VIN`, `READ_VOUT`, `READ_IIN`, `READ_IOUT`,
   `READ_TEMPERATURE_1`, `READ_POUT`;
3. декодування сирих слів у milli-одиниці через `pmbus_linear11_to_milli()` і
   `pmbus_linear16_to_mv()`;
4. формування `telemetry_record_t` з полями `valid_mask`, `retries`, `read_ms`,
   `seq`, `ts_ms`;
5. неблокуюча відправка запису в IPC-чергу з обліком втрат при переповненні.

Фрагмент коду 3.7 - цикл читання телеметрії та публікація в IPC-чергу
(`pmbus_poll_task.c`):

```c
if (read_cmd(addr, PMBUS_CMD_READ_VIN, &raw, &total_retries))
{
    rec.raw_vin = raw;
    rec.vin_mV  = pmbus_linear11_to_milli(raw);
    rec.valid_mask |= TELEM_VALID_VIN;
}

if (read_cmd(addr, PMBUS_CMD_READ_VOUT, &raw, &total_retries))
{
    rec.raw_vout = raw;
    rec.vout_mV  = pmbus_linear16_to_mv(raw, state->vout_exponent);
    rec.valid_mask |= TELEM_VALID_VOUT;
}

rec.seq   = gateway_ipc_next_seq();
rec.ts_ms = gateway_ipc_now_ms();

if (xQueueSend(gateway_ipc_telemetry_queue(), &rec, 0) != pdTRUE)
{
    metrics_inc_queue_drops();
}
```

Окремо задача виконує опитування статусних регістрів (`STATUS_WORD`,
`STATUS_VOUT`, `STATUS_IOUT`, `STATUS_TEMPERATURE`) і веде облік переходів
пристрою `ONLINE/OFFLINE` за критерієм послідовних невдалих циклів. Такий
підхід зменшує "флікер" стану при короткочасних збоях на шині та полегшує
аналіз експериментальних логів.

Отже, програмна реалізація 3.3 забезпечує повний ланцюг
`PMBus transaction -> decode -> telemetry/status record -> IPC queue`, причому
контур має вбудовані механізми захисту від помилок (retry, timeout, PEC,
optional bus-recovery) і метрики якості обміну [2], [4], [5], [10].

[[ВСТАВИТИ РИСУНОК 3.3 ТУТ]]
Підпис: Рисунок 3.3 - Блок-схема алгоритму `pmbus_poll_task`:
ініціалізація драйвера, опитування команд, декодування, формування записів,
передача в IPC та обробка помилок.
Файл: `docs/coursework/diagrams/exports/flow_pmbus_polling_task.png`.

## 3.4. Алгоритм мережевої взаємодії та формування JSON-контрактів

Мережеву взаємодію в gateway реалізує задача `mqtt_gw_task`, яка працює як
єдиний publisher для всіх типів повідомлень (telemetry, status, events,
metrics). Така серіалізація публікацій спрощує контроль QoS, облік помилок та
recovery-сценарії при втраті каналу зв'язку [6], [11].

У runtime ця задача виконує чіткий життєвий цикл:

1. ініціалізація Wi-Fi підсистеми (`cy_wcm_init`);
2. підключення до точки доступу (`wifi_connect`);
3. ініціалізація MQTT-бібліотеки й створення MQTT instance;
4. з'єднання з broker;
5. робочий цикл: drain IPC queues -> publish JSON -> flush buffer ->
   publish metrics.

Фрагмент коду 3.8 - connect/reconnect контур у `mqtt_gw_task.c`:

```c
backoff_reset();
for (;;)
{
    if (cy_wcm_is_connected_to_ap() == 0 && !wifi_connect())
    {
        backoff_wait();
        continue;
    }

    wallclock_sntp_init();

    if (!mqtt_lib_ready && !mqtt_init_and_create())
    {
        backoff_wait();
        continue;
    }

    if (!mqtt_broker_connect())
    {
        backoff_wait();
        continue;
    }

    gateway_ipc_set_mqtt_online(true);
    backoff_reset();
    break;
}
```

У робочому циклі підтримується реакція на розрив сесії через callback-прапор
`s_disconnect_pending`; після цього задача переходить у reconnect-шлях із
exponential backoff. Це відповідає практиці побудови стійких MQTT-клієнтів у
нестабільних IoT-мережах [6], [12], [13].

Фрагмент коду 3.9 - exponential backoff (`mqtt_gw_task.c`):

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

Модель даних у задачі публікації побудована як перетворення типізованих записів
із IPC-черг у JSON-payload. Для телеметрії ланцюг виглядає так:

`telemetry_record_t -> encode_telemetry_json() -> build_device_topic() -> publish_json()`.

Фрагмент коду 3.10 - обробка telemetry-черги (`mqtt_gw_task.c`):

```c
int json_len = encode_telemetry_json(&rec, s_json_buf, JSON_BUF_SIZE);
int topic_len = build_device_topic(s_topic_buf, TOPIC_BUF_SIZE,
                                   rec.addr_7bit, "telemetry");

if (!publish_json(s_topic_buf, s_json_buf, (size_t)json_len))
{
    buffer_mgr_put(s_topic_buf, s_json_buf, (uint16_t)json_len);
}
```

Формат JSON для telemetry/status/events/metrics задається окремими
кодерами (`telemetry.c`, `events.c`, `metrics.c`) і не потребує динамічних
алокацій під час серіалізації (формування через `snprintf`) [7].

Фрагмент коду 3.11 - частина telemetry JSON-контракту (`telemetry.c`):

```c
buf_printf(&pos, end,
    "{\"ts_ms\":%s,\"time_synced\":%s,\"seq\":%u,"
    "\"gw_id\":\"%s\",\"addr\":\"0x%02X\",\"label\":\"%s\","
    "\"pec\":%s,\"read_ms\":%u,\"retries\":%u",
    ts_buf, rec->time_synced ? "true" : "false",
    (unsigned)rec->seq, g_config.gw_id, (unsigned)rec->addr_7bit,
    rec->label ? rec->label : "?", rec->pec ? "true" : "false",
    (unsigned)rec->read_ms, (unsigned)rec->retries);
```

Topic-адресація будується від `base_topic` активного профілю:

1. telemetry/status: `<base_topic>/dev/0x<addr>/<suffix>`;
2. events: `<base_topic>/events`;
3. metrics: `<base_topic>/metrics`.

Фрагмент коду 3.12 - побудова device topic (`telemetry.c`):

```c
int len = snprintf(out, out_sz, "%s/dev/0x%02X/%s",
                   g_config.mqtt.base_topic,
                   (unsigned)addr_7bit,
                   suffix);
```

Публікація виконується через єдиний helper `publish_json_qos()`, який:

1. передає payload у `cy_mqtt_publish` із заданим QoS;
2. реєструє час операції в метриках;
3. інкрементує лічильники `mqtt_pub_ok/mqtt_pub_fail`.

Для telemetry/status/events невдала публікація переводить запис у буфер
store-and-forward (детальна реалізація наведена в підрозділі 3.5). Для metrics
застосовано іншу політику: метрики не буферизуються, оскільки при затримці
втрачають актуальність.

Отже, мережевий контур 3.4 реалізує стійкий шаблон
`queue -> JSON -> MQTT publish -> fallback`, сумісний із моделлю
publish/subscribe MQTT і вимогами до легковагового обміну JSON-даними у
вбудованих системах [6], [7], [11].

[[ВСТАВИТИ РИСУНОК 3.4 ТУТ]]
Підпис: Рисунок 3.4 - Блок-схема алгоритму `mqtt_gw_task`:
connect/reconnect, обробка IPC-черг, публікація JSON і fallback у буфер.
Файл: `docs/coursework/diagrams/exports/flow_mqtt_publish_reconnect.png`.

## 3.5. Програмна реалізація підсистеми буферизації

Підсистема буферизації реалізує шаблон store-and-forward для сценаріїв
нестабільного або відсутнього мережевого підключення: дані, які не вдалося
опублікувати в MQTT, не втрачаються одразу, а зберігаються локально до моменту
відновлення каналу [6], [12], [13].

У проєкті використано дворівневу модель:

1. рівень 1 - RAM ring buffer (`buffer_mgr.*`, швидкий і volatile);
2. рівень 2 - flash persistent buffer (`flash_buffer.*`, повільніший, але
   персистентний між перезавантаженнями).

За замовчуванням (профіль `default`) активовано RAM-рівень, а Flash-рівень
може бути ввімкнений конфігурацією `flash_max_records > 0`.

RAM-буфер організовано як класичне кільце з трьома індексними змінними:
`head` (позиція запису), `tail` (позиція читання), `count` (поточна глибина).
Пам'ять під масив записів виділяється один раз під час `buffer_mgr_init()` через
`pvPortMalloc`, розмір визначається профілем (`ram_max_records`) [8], [9].

Фрагмент коду 3.13 - структура RAM-кільця і ініціалізація (`buffer_mgr.c`):

```c
static buffer_record_t *s_ring = NULL;
static uint16_t s_capacity = 0u;
static uint16_t s_head = 0u;
static uint16_t s_tail = 0u;
static uint16_t s_count = 0u;

s_ring = (buffer_record_t *)pvPortMalloc(
    (size_t)s_capacity * sizeof(buffer_record_t));
```

Алгоритм запису `buffer_mgr_put()` має кілька кроків захисту від переповнення:

1. швидка перевірка доступності RAM-рівня;
2. якщо RAM повний, спроба spill у flash (`flash_buffer_put`) поза critical
   section;
3. якщо flash недоступний або теж повний - політика `drop_oldest` або
   `drop_newest` за конфігурацією;
4. запис topic/payload у head-слот і просування індекса.

Фрагмент коду 3.14 - overflow policy у `buffer_mgr_put()` (`buffer_mgr.c`):

```c
if (s_count >= s_capacity)
{
    taskEXIT_CRITICAL();

    if (g_config.buffer.flash_max_records > 0u)
    {
        bool spilled = flash_buffer_put(topic, payload, payload_len);
        if (spilled) { return true; }
    }

    taskENTER_CRITICAL();
    if (s_count >= s_capacity)
    {
        if (g_config.buffer.drop_oldest)
        {
            s_tail = (s_tail + 1u) % s_capacity;
            s_count--;
        }
        else
        {
            taskEXIT_CRITICAL();
            return false;
        }
    }
}
```

Для збереження FIFO-семантики при вивантаженні використовується двоетапна
операція `peek -> consume`:

1. `buffer_mgr_peek()` копіює найстаріший запис без видалення;
2. `buffer_mgr_consume()` видаляє його лише після успішної публікації.

Це виключає втрату запису між моментом читання буфера і фактичною відправкою в
мережу. Синхронізація доступу до RAM-індексів реалізована короткими критичними
секціями `taskENTER_CRITICAL/taskEXIT_CRITICAL` [8], [9].

Flash-рівень реалізовано на Em_EEPROM області (`32 KB`, адреса `0x14000000`):

1. row 0 - метадані кільця (`head`, `tail`, `count`, `version`, `crc32`);
2. rows 1..63 - дані записів (по одному запису на рядок 512 B);
3. кожен data-row має `magic`, `payload_len`, `topic`, `payload`, `crc32`.

Під час `flash_buffer_init()` відбувається відновлення метаданих із валідацією
`magic + CRC32`; у випадку невалідного стану створюється "fresh" метадані.
Під час `flash_buffer_peek()` додатково перевіряється цілісність data-row; якщо
рядок пошкоджений, tail автоматично зсувається далі, щоб уникнути зациклення на
битому записі.

Фрагмент коду 3.15 - перевірка та автопропуск невалідного flash-запису
(`flash_buffer.c`):

```c
if (!record_is_valid(flash_rec))
{
    s_meta.tail = (s_meta.tail + 1u) % capacity;
    s_meta.count--;
    (void)meta_write();   /* best-effort persist */
    metrics_inc_buffer_dropped();
    return false;
}
```

Важливо, що `buffer_task` у поточній архітектурі не виконує `cy_mqtt_publish()`.
Усі публікації централізовано в `mqtt_gw_task`, а `buffer_task` виконує
housekeeping:

1. періодичне оновлення gauge-метрик глибини RAM/Flash;
2. підтримка фонової діагностики буфера.

Фактичне вивантаження буфера відбувається у `mqtt_gw_task` батчами:
спочатку Flash FIFO (старіші записи), потім RAM FIFO. Це зберігає часовий
порядок доставки при recovery-сценаріях.

Отже, підсистема 3.5 забезпечує контрольовану деградацію і відновлення:
при втраті MQTT-зв'язку записи можуть бути перенаправлені до RAM/Flash-буфера
з журналюванням подій та метрик [8], [9]. Разом з тим для поточної coursework-
версії зафіксовано обмеження: при довгому outage до відновлення broker частина
live telemetry може втрачатися через переповнення telemetry queue, тому
повністю безвтратний recovery-сценарій потребує окремого hardening-етапу.

[[ВСТАВИТИ РИСУНОК 3.5 ТУТ]]
Підпис: Рисунок 3.5 - Алгоритм store-and-forward: RAM ring buffer,
spill у Flash, recovery-flush за схемою `peek -> publish -> consume`.
Файл: `docs/coursework/diagrams/exports/flow_store_and_forward_buffer.png`.

## 3.6. Висновки до розділу 3

У Розділі 3 виконано детальне проєктно-технологічне опрацювання програмного
забезпечення мікрокомп'ютера gateway: від середовища розробки і bootstrap-етапу
до реалізації ключових runtime-алгоритмів збору, передавання та збереження
телеметрії.

За результатами розділу встановлено:

1. модуль ініціалізації (`main`) забезпечує керований старт системи за
   fail-fast підходом із запуском FreeRTOS-задач у визначеній пріоритетній
   моделі;
2. PMBus-підсистема (`pmbus_master` + `pmbus_poll_task` + `pmbus_decode`)
   реалізує повний цикл `read -> decode -> record -> queue` з механізмами
   retry/timeout/PEC і контролем стану пристрою;
3. мережевий контур (`mqtt_gw_task`) реалізує стійку публікацію JSON-повідомлень
   у MQTT з reconnect/exponential-backoff та контрольованою реакцією на
   розриви з'єднання;
4. підсистема буферизації (RAM/Flash store-and-forward) забезпечує
   безперервність збору даних у offline-сценаріях і FIFO-відновлення доставки
   після повернення мережі.

Отже, програмна реалізація відповідає архітектурним рішенням Розділу 2 та
функціональним вимогам технічного завдання, а також формує достатню практичну
основу для переходу до Розділу 4, де буде наведено методику та результати
тестування розробленого ПЗ.

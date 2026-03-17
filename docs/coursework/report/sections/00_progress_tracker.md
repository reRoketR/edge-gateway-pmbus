# Report Progress Tracker

| Section | File | Status | Notes |
|---|---|---|---|
| Technical Assignment | `01_technical_assignment.md` | Draft v1 | Перший повний шматок, готується до стилістичного шліфування |
| Introduction | `02_introduction.md` | Draft v1 | Детальний вступ (актуальність, мета, задачі, об'єкт/предмет) |
| Section 1: Functional Scheme | `03_section1_functional_scheme.md` | Draft v2 | Прийнято референс-структуру 1.1-1.4, додано місця вставки рис. 1.1-1.3 |
| Section 2: Architecture | `04_section2_architecture.md` | Draft v2 | Розширено обсяг, додано 5 код-сніпетів (tasks/IPC/publish/flush/backoff) |
| Section 3: Software | `05_section3_software.md` | Backlog | Після Section 2; мапінг `init/driver/algorithm` |
| Section 4: Testing | `06_section4_testing.md` | Backlog | Після Section 3; методика + pass/fail + докази |
| Conclusions | `07_conclusions.md` | Backlog | Пишеться після стабілізації розділів 1-4 |

## Next Iteration

1. Експортувати UML-діаграми для Розділу 2 (`uml_component`, `uml_sequence_pmbus_to_mqtt`).
2. Узгодити остаточний набір рисунків Розділу 1 (чи залишаємо рис. 1.3 окремо).
3. Перейти до повного написання Розділу 3 на базі коду `main/pmbus_poll/mqtt_gw`.

## Deferred For Bachelor's

1. Описати й запланувати SMBus timeout-driven recovery через hardware timer на
   лінії `SCL`: timeout monitor, ISR-based abort/re-init, optional active bus
   reset via forced `SCL low`.
2. Не включати цю фічу в обов'язковий scope курсової; повертатися до неї в
   бакалаврській лише якщо потрібно показати advanced hot-plug / bus recovery
   robustness.

## Citation Policy

1. Посилання `[n]` використовувати тільки на зовнішню літературу/стандарти/статті.
2. Внутрішні файли `docs/*.md` використовувати як артефакти проєкту без `[n]`.

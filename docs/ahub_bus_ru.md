# Документация: `ahub_bus` (`src/ahub_bus.cpp/.h`)

## Назначение
`ahub_bus` реализует протокол AHUB (кадры `0x33...`) со стороны BMCU-узла:
- heartbeat-ответы о доступных AMS;
- query-ответы (имя AMS, filament info/status, dryer status, all_filament_status);
- set-команды от хоста (обновление filament info, dryer, массовый статус).

См. также:
- [BambuBus AMS](./bambu_bus_ams_ru.md)
- [Motion control](./motion_control_ru.md)

## Публичный интерфейс
- `void ahubus_init()` — включает тактирование CRC-периферии MCU (`RCC_AHBPeriph_CRC`).
- `ahubus_package_type ahubus_run()` — шаг обработки AHUB.
- `enum class ahubus_package_type` — `heartbeat`, `query`, `set`, `none`, `error`.
- `enum class ahubus_set_type` — типы set-команд (`filament_info`, `dryer_stu`, `all_filament_stu`).

## Проверка пакета и CRC
`ahubus_get_package_type(buf)`:
1. проверка magic `0x33`;
2. расчёт CRC через аппаратный модуль CRC по 32-битным словам;
3. сравнение с CRC в хвосте;
4. возврат типа команды (`buf[4]`).

Формирование исходящих пакетов:
- `ahubus_package_add_crc(buf)`:
  - считает `crc8` заголовка (`bus_crc8(buf, 3)`);
  - считает и дописывает 32-битный CRC блока;
  - возвращает итоговую длину в байтах.

## Heartbeat
`ahubus_slave_get_package_heartbeat(buf)`:
- формирует ответ с перечнем online AMS;
- для каждого online-модуля пишет пару `{adr, ams_type}`;
- выравнивает структуру до 4 байт при необходимости;
- отправляет пакет.

## Query
`ahubus_slave_get_package_query(buf)` обслуживает типы:
- `ams_name` — 8 байт имени;
- `filament_info` — 4×44 байта ID/метаданных каналов;
- `filament_stu` — 4 структуры по 8 байт (motion, seal, температура, влажность, dryer...);
- `dryer_stu` — компактно по каналам;
- `all_filament_stu` — агрегированный список по всем online AMS.

Особенность `xMCU`:
- адресация в пакете может использовать старшие 4 бита (`adr >> 4`), это учитывается при разборе.

## Set
`ahubus_slave_get_package_set(buf)` принимает:
- `filament_info`: обновляет `ams[set_adr].filament[ch].bambubus_filament_id`, и если это локальный AMS (`BAMBU_BUS_AMS_NUM`) — ставит флаг сохранения во Flash;
- `dryer_stu`: обновляет блок параметров dryer для канала;
- `all_filament_stu`: массово применяет `now_filament_num` и motion-состояния.

После обработки отправляет ack-пакет set-типа.

## Главный цикл `ahubus_run()`
1. атомарно читает RX-поля из `bus_port_to_host`;
2. проверяет, что пришёл именно AHUB-пакет (`_bus_data_type::ahub_bus`);
3. классифицирует пакет;
4. dispatch на heartbeat/query/set обработчик;
5. очищает RX-состояние;
6. проверяет heartbeat deadline (1 сек), при таймауте возвращает `ahubus_package_type::error`.

## Интеграционные моменты
- `ahub_bus` работает поверх общего транспортного слоя `_bus_hardware` и разделяет TX с `bambu_bus_ams`.
- Модуль активно читает/меняет общую модель `ams[]`, поэтому фактическое движение/pressure потом отражается в `Motion_control` и BambuBus-ответах.

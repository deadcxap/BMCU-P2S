# Документация: `bambu_bus_ams` (`src/bambu_bus_ams.cpp/.h`)

## Назначение модуля
`bambu_bus_ams` — это протокольный адаптер между BMCU и принтером по шине BambuBus (кадры `0x3D...`).
Модуль:
- принимает кадры от принтера;
- проверяет CRC8/CRC16;
- классифицирует тип пакета;
- обновляет состояние `ams[]` (текущий канал, фазы подачи/отката, pressure, loaded/unloaded);
- формирует ответные short/long пакеты.

Связанные документы:
- [AHUB-шина](./ahub_bus_ru.md)
- [Motion control](./motion_control_ru.md)
- [Сценарий загрузки пластика / смены цвета](./filament_load_sequence_ru.md)

## Публичный интерфейс (`bambu_bus_ams.h`)
- `enum class bambubus_package_type` — классификация входящих пакетов (motion, heartbeat, online detect, version, serial, p2s и пр.).
- `bambubus_package_type bambubus_run()` — один шаг обработки шины.
- `uint16_t bambubus_ams_address` — активный target-адрес протокола AMS (используется для режимов AMS/P2S).

## Структуры и формат пакетов
### 1) Long packet envelope
`bambubus_long_packge_data` описывает long-кадр:
- `package_number`, `package_length`, `crc8`;
- `target_address`, `source_address`;
- `type`;
- `datas` + `data_length`.

Функции:
- `bambubus_long_package_analysis(...)` — разбор входящего long-пакета в структуру.
- `bambubus_long_package_get(...)` — сборка и постановка в TX очереди.

### 2) Motion short/long
- `bambubus_printer_motion_package_struct` (команда `0x03`) — команда движения от принтера.
- `bambubus_ams_motion_package_struct` — короткий ответ AMS с текущим `filament_use_flag`, `filament_channel`, `meters`, `pressure`.
- `bambubus_printer_stu_motion_package_struct` (команда `0x04`) — расширенный motion-запрос.
- `bambubus_ams_stu_motion_package_struct` — расширенный ответ (температура/влажность + motion-данные).

### 3) Online/Version/Serial/Filament info
Модуль содержит готовые шаблоны payload (online detect, version/name, serial и т.п.), которые заполняются по актуальному состоянию `ams`.

## CRC и валидация
- `package_check_crc16(data, len)` — проверка хвостового CRC16.
- `package_add_crc(data, send_len)` — пересчёт CRC8 заголовка и CRC16 хвоста для short/long формата.

## Классификация входящих пакетов
`get_packge_type(buf, len)`:
1. проверяет magic `0x3D`, минимальную длину, CRC16;
2. разделяет short (`flag` вида `0xC5`) и long (`0x05/0x04`);
3. для short возвращает типы `filament_motion_short`, `heartbeat`, `online_detect`, `set_filament_info` и др.;
4. для long парсит `printer_data_long.type` и возвращает `MC_online`, `read_filament_info`, `set_filament_info_type2`, `version`, `serial_number`, `p2s_long_0237`, `p2s_long_023c`.

## Логика движения и синхронизации состояния
### Ключевая функция: `set_motion(read_num, statu_flags, fliment_motion_flag, ams_num)`
Назначение: преобразовать протокольные флаги принтера в внутренние состояния `ams[...].filament[ch].motion`.

Основные события, которые распознаются:
- `send_out`;
- `before_on_use`;
- `stop_on_use`;
- `on_use`;
- `before_pull_back`;
- `pull_back` (через `read_num == 0xFF` + определённый `statu_flags`).

Что ещё делает функция:
- синхронизирует `now_filament_num`;
- поддерживает `filament_use_flag` и `pressure`;
- обновляет persistent-state загрузки через `ams_state_set_loaded()/ams_state_set_unloaded()`;
- ведёт тайминг перехода `send_out -> on_use` (`time_sendout_onuse_ticks[]`) для корректной стадии `printing` в P2S.

## P2S-режим
В модуле есть отдельная ветка для P2S:
- `p2s_mode`, `p2s_state` (`boot/runtime/printing`);
- последовательности ответов `p2s_a0_payload_seq`, `p2s_0237_*`, `p2s_023c_*`;
- обработчики `get_package_p2s_short_a0`, `get_package_p2s_long_0237`, `get_package_p2s_long_023c`.

Это эмуляция ожидаемого протокольного поведения AMS на этапах запуска/рантайма/печати.

## Обработчики команд
- `get_package_motion(...)` — обработка short motion `0x03`, ответ short motion status.
- `get_package_stu_motion(...)` — обработка short motion `0x04`, ответ расширенного статуса.
- `get_package_online_detect(...)` — регистрация AMS и подтверждение online detect.
- `get_package_long_packge_MC_online(...)` — ответ на long MC online.
- `get_package_long_packge_filament(...)` — выдача сведений о конкретной катушке/канале.
- `get_package_long_packge_version(...)` / `...serial_number(...)` — версия и серийник.
- `get_package_set_filament(...)` / `...type2(...)` — принятие новых filament-метаданных от принтера и подтверждение.

## Главный цикл: `bambubus_run()`
Алгоритм:
1. атомарно снимает указатели/длину принятых данных из `bus_port_to_host`;
2. если принят кадр BambuBus — определяет `bambubus_package_type`;
3. вызывает соответствующий обработчик;
4. при наличии ответа даёт короткую паузу (`delay_us(50)`);
5. очищает RX-состояние буфера;
6. следит за heartbeat deadline (1 сек) и в случае таймаута возвращает `error`.

## Что важно для интеграции
- Все ответы отправляются только если `bus_port_to_host.send_data_len == 0` (модуль не перетирает уже сформированный TX).
- Любой пакет с битой CRC игнорируется на этапе классификации.
- Состояние движения (и в итоге моторная реакция) транслируется в `ams[]`, а фактическую механику выполняет `Motion_control`.

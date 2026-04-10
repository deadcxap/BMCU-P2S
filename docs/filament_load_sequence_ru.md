# Процедура загрузки пластика в начале печати и при смене цвета

Документ описывает сквозной сценарий: что делает BMCU, что получает от принтера по BambuBus, что отправляет обратно, и где это можно настраивать.

Связанные модули:
- [BambuBus AMS](./bambu_bus_ams_ru.md)
- [Motion control](./motion_control_ru.md)
- [MC_PULL calibration](./mc_pull_calibration_ru.md)

## 1) Общая последовательность обмена
1. Принтер отправляет motion-команды (`0x03`/`0x04`) и heartbeat по BambuBus.
2. `bambu_bus_ams` декодирует пакет и переводит флаги в внутренние состояния:
   - `send_out` -> подача,
   - `before_on_use` -> доводка к рабочей зоне,
   - `on_use` -> рабочее удержание,
   - `before_pull_back`/`pull_back` -> подготовка и откат.
3. Обновлённые состояния записываются в `ams[]`.
4. `Motion_control_run()` читает `ams[]`, датчики ADC/AS5600 и исполняет физическое движение мотора.
5. `bambu_bus_ams` отправляет в ответ текущие параметры:
   - канал,
   - `filament_use_flag`,
   - `meters`,
   - `pressure`,
   - online/status flags.

## 2) Что делает BMCU в фазе загрузки (load)
## Фаза A: `send_out`
- Назначение: протолкнуть филамент вперёд.
- Реакция BMCU:
  - перевод канала в state подачи (`filament_motion_send`),
  - speed-PID + ограничения soft-start/ramp,
  - hard-stop по порогу давления (`MC_LOAD_S1_HARD_STOP_PCT`),
  - ограничение длины подачи (`SEND_MAX_M`) как предохранитель.

## Фаза B: `before_on_use`
- Назначение: стабильно дотянуть давление к целевой зоне.
- Реакция BMCU:
  - алгоритм `hold_load(...)`:
    - если давление выше окна — контролируемый retract,
    - если ниже — push PWM по линейному профилю,
    - в окне — мотор остановлен.

## Фаза C: `on_use`
- Назначение: рабочее состояние во время печати.
- Реакция BMCU:
  - контур `filament_motion_pressure_ctrl_on_use`;
  - поддержание target pressure диапазона;
  - антизакусывание/anti-stall;
  - jam-детект с установкой `pressure = 0xF06F` и красной индикацией.

## 3) Что происходит при смене цвета (unload -> load)
Типовой сценарий:
1. Принтер переводит активный канал в `before_pull_back`.
2. BMCU аккуратно набирает «предоткат» и учитывает уже откачанную длину.
3. Принтер даёт `pull_back`, BMCU выполняет откат на целевую длину (`AMS_RETRACT_LEN`, с поправкой на уже набранный before_pull_back).
4. В `redetect` BMCU дожидается корректного состояния key.
5. После выбора нового канала цикл снова идёт через `send_out -> before_on_use -> on_use`.

## 4) Что BMCU отправляет принтеру
В short/long ответах BMCU передаёт:
- `filament_use_flag` (фаза использования);
- `filament_channel`;
- `meters` (накопленная длина по AS5600);
- `pressure` (в т.ч. спец-код jam `0xF06F`);
- флаги online-каналов и motion-состояний;
- для отдельных команд — filament metadata, version, serial.

## 5) Что BMCU получает от принтера
Ключевые входящие сущности:
- heartbeat — признак живой сессии;
- short motion (`0x03`) и status-motion (`0x04`) с полями:
  - `ams_num`,
  - `statu_flag`,
  - `filament_channel`/`read_num`,
  - `motion_flag`;
- online detect/registration;
- set/read filament info;
- P2S-специфичные запросы (`A0`, `0x0237`, `0x023c`).

## 6) Где настраивать поведение
## Compile-time параметры (PlatformIO)
- `BAMBU_BUS_AMS_NUM` — номер локального AMS.
- `AMS_RETRACT_LEN` — длина отката на unload/смене цвета.
- `BMCU_DM_TWO_MICROSWITCH` — включает Stage1/Stage2 autoload с двумя микриками.
- `BMCU_ONLINE_LED_FILAMENT_RGB` — LED-цвет катушки в loaded.
- Профили `BMCU_SOFT_LOAD` / `BMCU_P1S` / default(A1) меняют пороги и PWM кривые.

## Калибровка/Flash
- `MC_PULL_V_OFFSET/MIN/MAX`, `MC_PULL_POLARITY` — влияют на точность pressure-процента.
- `MC_DM_KEY_NONE_THRESH` — определяет, когда key считается «пусто».
- Оба набора параметров сохраняются в Flash и используются на каждом старте.

## 7) Реакция на ошибки
- Timeout heartbeat на BambuBus -> модуль шины возвращает `error`, и motor-loop останавливает активное движение.
- Потеря AS5600 (offline/magnet error) -> принудительная остановка канала + красная индикация.
- Jam (on_use) -> latch + остановка + `pressure=0xF06F` для сигнализации принтеру.

## 8) Кратко: кто за что отвечает
- `bambu_bus_ams`: протокол и state mapping команд принтера.
- `Motion_control`: фактическая моторика и защитные алгоритмы.
- `MC_PULL_calibration`: качество измерений, от которых зависят решения о push/pull/stop.

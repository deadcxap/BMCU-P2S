# Документация: `Motion_control` (`src/Motion_control.cpp/.h`)

## Роль модуля
`Motion_control` — центральная логика механики подачи/удержания/отката филамента.
Модуль связывает:
- входные датчики (ADC + AS5600),
- состояние протокола в `ams[]` (которое задаёт `bambu_bus_ams`),
- управление PWM-выходами моторов,
- RGB-индикацию каналов,
- сохранение calibration/настроек в Flash.

См. также:
- [Калибровка MC_PULL](./mc_pull_calibration_ru.md)
- [BambuBus AMS](./bambu_bus_ams_ru.md)
- [Сценарий загрузки пластика / смены цвета](./filament_load_sequence_ru.md)

## Публичные API (`Motion_control.h`)
- `void Motion_control_init()` — инициализация датчиков, моторов, начальных состояний.
- `void Motion_control_run(int error)` — основной шаг control-loop.
- `void Motion_control_set_PWM(uint8_t CHx, int PWM)` — низкоуровневая установка PWM направления/силы для канала.
- `bool Motion_control_save_dm_key_none_thresholds()` — сохранение порогов DM key в Flash.
- `void MC_PULL_detect_channels_inserted()` — проверка, какие каналы физически подключены.

Экспортируемые параметры:
- `MC_PULL_V_OFFSET/MIN/MAX` — калибровка pull-датчиков;
- `MC_PULL_POLARITY` — полярность канала;
- `MC_PULL_pct` — нормализованный pressure-процент;
- `MC_DM_KEY_NONE_THRESH` — порог «нет филамента» для DM key;
- `filament_channel_inserted` — флаг физического наличия канала.

## Конфигурация compile-time
Ключевые макросы:
- `BAMBU_BUS_AMS_NUM` — локальный индекс AMS (0..3);
- `AMS_RETRACT_LEN` — длина штатного отката при pull_back (м);
- `BMCU_DM_TWO_MICROSWITCH` — расширенная логика двух микриков + autoload;
- `BMCU_ONLINE_LED_FILAMENT_RGB` — показ цвета филамента на ONLINE LED;
- профиль принтера через `BMCU_SOFT_LOAD`/`BMCU_P1S`/default (A1), который меняет пороги control-петли.

## Блоки модуля
## 1) Считывание и нормализация датчиков
### `MC_PULL_ONLINE_read(...)`
- читает 8 ADC-каналов;
- раскладывает их в pull/key по каналам 0..3;
- применяет `offset` и `polarity`;
- переводит pull-напряжение в проценты (0..100) с использованием калиброванных `V_MIN/V_MAX`;
- формирует дискретное состояние `MC_PULL_stu` (-1/0/+1) с deadband;
- поддерживает `MC_ONLINE_key_stu` (присутствие/состояние филамента по key-каналу).

### Детект подключённого канала
`MC_PULL_detect_channels_inserted()` усредняет ADC и помечает канал «вставлен», если напряжение в допустимом диапазоне.

## 2) Моторный контур
### Класс `_MOTOR_CONTROL`
Для каждого канала хранит:
- текущее motor-state (`filament_motion_enum`);
- PID-регуляторы скорости и давления;
- таймеры переходов и защиты;
- параметры send/pull фаз;
- anti-jam latch (`g_on_use_low_latch`, `g_on_use_jam_latch`).

Основной метод: `run(time_E, now_ms)`.

### Режимы/состояния управления
Внутренние motion-state:
- `filament_motion_send` — активная подача;
- `filament_motion_before_on_use` — переход к «рабочему» удержанию;
- `filament_motion_pressure_ctrl_on_use` — рабочий контур on_use;
- `filament_motion_stop_on_use` — пауза в on_use;
- `filament_motion_before_pull_back` — подготовка к откату;
- `filament_motion_pull` — откат;
- `filament_motion_redetect` — повторный поиск состояния после отката;
- `filament_motion_pressure_ctrl_idle` — idle-контур удержания/нейтрали.

### Алгоритмы
- Speed PID для send/pull.
- Pressure PID для idle/on_use удержания.
- `hold_load(...)` — общая логика стадии «дотягивания/удержания» после send_out.
- Anti-stall/anti-jam:
  - kick PWM при залипании;
  - временная блокировка канала;
  - jam-сигнализация через `pressure = 0xF06F`.
- Ramping (ограничение прироста PWM по времени) для мягких переходов.

## 3) AS5600: скорость и длина
`AS5600_distance_updata(...)`:
- периодически опрашивает AS5600;
- валидирует датчики (good/fail streak);
- вычисляет diff угла с учётом wrap-around;
- переводит в мм и мм/с;
- обновляет `ams[...].filament[i].meters`.

## 4) Машина логики филамента (верхний уровень)
### `motor_motion_switch(...)`
Сопоставляет внешние состояния `_filament_motion` (из `ams[]`, их задаёт протокол BambuBus) с внутренними motor-state.

Например:
- `_filament_motion::send_out` -> `filament_motion_send`;
- `_filament_motion::before_on_use` -> `filament_motion_before_on_use`;
- `_filament_motion::on_use` -> `filament_motion_pressure_ctrl_on_use`;
- `_filament_motion::before_pull_back` -> `filament_motion_before_pull_back`;
- `_filament_motion::pull_back` -> `filament_motion_pull`.

### `motor_motion_filamnet_pull_back_to_online_key(...)`
Отдельно обрабатывает фазу отката и редетекта до возвращения в idle.

## 5) Спецрежим DM (две микрокнопки)
Если `BMCU_DM_TWO_MICROSWITCH=1`, включается расширенный автомат:
- распознавание состояний key: none / both / external;
- Stage1/Stage2 autoload;
- контролируемая подача на целевую длину (`DM_AUTO_S2_TARGET_M`), попытки recovery, fail-latch;
- блокировки/разблокировки через `dm_autoload_gate`, `dm_fail_latch`, `dm_loaded`.

## 6) LED-индикация
Модуль управляет LED каналов по приоритету:
1. аварии (jam/fail/AS5600 offline) — красный;
2. активные фазы motion — кодированные цвета;
3. idle/loaded/empty состояния;
4. опционально цвет филамента (`BMCU_ONLINE_LED_FILAMENT_RGB`).

## 7) Сохранение/восстановление настроек
- Структура `Motion_control_save_struct` хранит `Motion_control_dir[]` и `dm_key_none_cv[]`.
- `Motion_control_read()` читает из Flash, проверяет magic `check`.
- `Motion_control_save()` сохраняет направление моторов и пороги DM key.

## 8) Инициализация
`Motion_control_init()`:
- поднимает AMS online/type;
- читает сохранённые настройки;
- детектирует подключённые каналы;
- инициализирует AS5600 и моторы;
- определяет направление каждого мотора (`MOTOR_get_dir()`), затем применяет PWM-нули.

## 9) Основной цикл
`Motion_control_run(error)`:
1. обновляет ADC-состояние (`MC_PULL_ONLINE_read`);
2. синхронизирует loaded/unloaded статус;
3. обрабатывает jam-условия и reset-гест калибровки;
4. обновляет AS5600 distance/speed;
5. поддерживает `filament.online`;
6. исполняет `motor_motion_run(...)` для каждого канала;
7. накладывает финальную индикацию ошибок.

## Что настраивается
Через конфигурацию и калибровку можно регулировать:
- целевые пороги удержания/push для A1/P1S/soft-load;
- длину pull_back (`AMS_RETRACT_LEN`);
- поведение DM autoload (тайминги/пороги);
- пороги DM key none;
- цветовую индикацию loaded filament.

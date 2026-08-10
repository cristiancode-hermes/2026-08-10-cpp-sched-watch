# cpp-sched-watch — Scheduler Watch (Linux)

Monitor del **scheduler de Linux** en **C++23** que lee `/proc/stat`, `/proc/schedstat`, `/proc/loadavg` y `/proc/uptime` para mostrar la presión real del planificador: load averages, context switches, forks de procesos, run queue y tiempo runnable/blocked por CPU. Cero dependencias externas: compilación directa con `g++`.

## Features

- Load averages 1/5/15 min + tasks running/total + último PID (`/proc/loadavg`)
- Contadores globales: context switches (`ctxt`), forks (`processes`), procs_running/blocked y boot time (`btime`) desde `/proc/stat`
- **Presión del scheduler por CPU** desde `/proc/schedstat`: % de tiempo con tareas runnable/blocked en cola desde boot y switches totales por núcleo
- Modo **watch** (`--watch N`): tasas delta en vivo cada N segundos (ctxt/s, forks/s) con refresco en pantalla
- Salida **JSON** completa para scripting (`--json`) con estructura `{loadavg, counters, rates, cpus[], summary}`
- Filtrado por presión con `--top N` (top N CPUs más ocupadas)
- Colores ANSI (desactivables con `--no-color`)
- 24 tests de auto-verificación (`--selftest`): parseo real de `/proc/stat` + `/proc/schedstat` + `/proc/loadavg`, derivación de métricas, formateo y validación estructural JSON

## Build

```bash
g++ -std=c++23 -O2 -Wall -Wextra src/main.cpp -o sched-watch
```

## Usage

```bash
./sched-watch                      # snapshot: promedios desde boot
./sched-watch --watch 2            # tasas delta en vivo cada 2s
./sched-watch --json               # JSON completo
./sched-watch --top 2              # solo las 2 CPUs con más presión
./sched-watch --no-color           # sin ANSI (pipes/logs)
./sched-watch --selftest           # auto-verificación
```

## Ejemplo

```
Scheduler Watch — /proc/stat + /proc/schedstat + /proc/loadavg  (rates: avg since boot, uptime 28d 08:08:33)
-------------------------------------------------------------------------------
LOAD AVERAGE      1min 0.13  5min 0.14  15min 0.10   1/731 tasks  last pid 599883
CONTEXT SWITCHES   total 2.83G   avg 1154.8/s
PROCESS FORKS      total 7.1M   avg 2.9/s
RUN QUEUE          running 1   blocked 0   avg runnable 13.5%  blocked 1.6%
BOOT TIME          2026-07-12 19:24:16 UTC   (uptime 28d 08:08:33)

PER-CPU SCHEDULER PRESSURE  (runnable + blocked queue time since boot)
CPU     RUNNABLE   BLOCKED   SWITCHES      SW/s
cpu0      13.5%     1.6%     400.9M    163.7
cpu1      13.5%     1.6%     395.8M    161.7
cpu2      13.5%     1.7%     398.2M    162.6
cpu3      13.6%     1.6%     397.1M    162.2
cpus: 4
```

## Fuentes de datos

| Archivo | Campos usados |
|---|---|
| `/proc/loadavg` | load1/5/15, running/total, last_pid |
| `/proc/stat` | ctxt, processes, procs_running, procs_blocked, btime |
| `/proc/schedstat` | runnable_ns, blocked_ns, nr_switches por CPU (campos 6-8) |
| `/proc/uptime` | uptime total para derivar % y tasas medias |

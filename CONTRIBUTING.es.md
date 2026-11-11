# 🛠️ Guía del Colaborador & Manual de Desarrollo (Onboarding)

> 🌐 **Language / Idioma:** [English](CONTRIBUTING.md) | **Español**

Bienvenido al repositorio **`forcelab-stm32h755-dualcore-workspace`**. Este espacio de trabajo está diseñado como un monorepo modular para desarrollo de firmware embebido en arquitecturas dual-core **STM32H755ZI** (Arm Cortex-M7 @ 480 MHz + Arm Cortex-M4 @ 240 MHz).

Esta guía te proporcionará el paso a paso exacto para configurar tu entorno, clonar dependencias, compilar firmware, flashear la placa y seguir el flujo de trabajo en Git.

---

## 📑 Tabla de Contenidos
1. [Requisitos Previos del Sistema](#1-requisitos-previos-del-sistema)
2. [Paso a Paso de Inicio Rápido (Quickstart)](#2-paso-a-paso-de-inicio-rápido-quickstart)
3. [Restauración del SDK STM32CubeH7 (¡Crítico!)](#3-restauración-del-sdk-stm32cubeh7-crítico)
4. [Compilación de Proyectos Dual-Core](#4-compilación-de-proyectos-dual-core)
5. [Flasheo, Monitorización y Pruebas en Hardware](#5-flasheo-monitorización-y-pruebas-en-hardware)
6. [Arquitectura y Reglas de Desarrollo](#6-arquitectura-y-reglas-de-desarrollo)
7. [Flujo de Trabajo en Git (Branching & Commits)](#7-flujo-de-trabajo-en-git-branching--commits)
8. [Resolución de Problemas Frecuentes (FAQ)](#8-resolución-de-problemas-frecuentes-faq)

---

## 1. Requisitos Previos del Sistema

Asegúrate de contar con las siguientes herramientas en tu máquina de desarrollo:

| Herramienta | Versión mínima | Propósito |
| :--- | :--- | :--- |
| **Git** | 2.30+ | Control de versiones (activar `core.longpaths` en Windows). |
| **GNU Arm Toolchain** | 12.3+ (`arm-none-eabi-gcc`) | Compilador cruzado para arquitecturas Cortex-M. |
| **CMake** | 3.22+ | Sistema de generación de compilación. |
| **Ninja** | 1.10+ | Generador de compilación de alta velocidad. |
| **Python** | 3.10+ | Para scripts de flasheo, monitorización serie y dashboards. |
| **VS Code** (Recomendado) | Actual | Editor de código configurado mediante workspace. |
| **STM32CubeProgrammer / OpenOCD** | Actual | Herramienta de flasheo para ST-Link V3. |

> 💡 **Tip para Windows:** Si tienes instalado STM32CubeIDE o STM32CubeCLT, puedes cargar automáticamente la toolchain al `PATH` ejecutando en PowerShell:
> ```powershell
> .\tools\env.ps1
> ```

---

## 2. Paso a Paso de Inicio Rápido (Quickstart)

```powershell
# 1. Clonar el repositorio
git clone https://github.com/hrodriguez06/forcelab-stm32h755-dualcore-workspace.git
cd forcelab-stm32h755-dualcore-workspace

# 2. Habilitar rutas largas en Windows (evita errores con paths de ST)
git config core.longpaths true

# 3. Inicializar el SDK de STM32CubeH7 de forma ligera
.\tools\init_cube_submodule.ps1

# 4. Abrir en VS Code con el workspace multi-root
code stm32h755.code-workspace
```

---

## 3. Restauración del SDK STM32CubeH7 (¡Crítico!)

> ⚠️ **ADVERTENCIA IMPORTANTE:**  
> **NUNCA ejecutes `git submodule update --init --recursive`.**  
> El paquete oficial de ST contiene más de 52 submódulos anidados y rutas de más de 260 caracteres que fallan en Windows, descargando además más de 350 MB innecesarios.

El repositorio incluye un script especializado:
```powershell
.\tools\init_cube_submodule.ps1
```
* **Qué hace:** Descarga de forma parcial (`--filter=blob:none`), somera (`--depth 1`) y dispersa (`sparse-checkout`) únicamente las carpetas `Drivers` (CMSIS, HAL) y `Middlewares` (FreeRTOS, LwIP) requeridas por el firmware.
* **Tiempo estimado:** ~20-30 segundos (solo ~26 MB frente a 359 MB).
* **Verificación:** Puedes chequear el estado sin alterar nada ejecutando:
  ```powershell
  .\tools\init_cube_submodule.ps1 -Check
  ```

---

## 4. Compilación de Proyectos Dual-Core

Los proyectos se encuentran en la carpeta `02_firmware_projects/`. Cada proyecto compila **ambos núcleos (CM7 y CM4)** en un único paso de configuración:

### Ejemplo: Compilar `prj_04_adc_dma_bm_eth`

```powershell
cd 02_firmware_projects/prj_04_adc_dma_bm_eth

# Configurar con Ninja
cmake -B build -G Ninja

# Compilar ambos firmwares
cmake --build build
```

### Usando CMake Presets (Alternativa recomendada):
```powershell
# Configuración y compilación en modo Debug
cmake --preset debug
cmake --build --preset debug

# O para Release
cmake --preset release
cmake --build --preset release
```

### Salida esperada en la carpeta `build/`:
* `cm7_firmware.elf` / `.bin`: Firmware para Cortex-M7 (asignado a Flash Bank 1: `0x08000000`).
* `cm4_firmware.elf` / `.bin`: Firmware para Cortex-M4 (asignado a Flash Bank 2: `0x08100000`).

---

## 5. Flasheo, Monitorización y Pruebas en Hardware

Conecta tu placa **NUCLEO-H755ZI-Q** por USB (puerto ST-LINK):

### A. Flasheo Dual-Core automático
Desde la raíz del proyecto específico o desde la raíz del workspace:
```powershell
# Flashea automáticamente CM7 en Bank 1 y CM4 en Bank 2
python tools/flash.py
```

### B. Monitor Serial ST-Link (VCP)
Abre la consola serie sin necesidad de programas externos (Putty/TeraTerm):
```powershell
python tools/monitor.py
```

### C. Prueba de Red Ethernet (Ping + UDP Echo)
Para proyectos que utilicen lwIP (ej. `prj_04`, `prj_05`, `prj_06`):
```powershell
python tools/net_check.py 192.168.1.50
```

---

## 6. Arquitectura y Reglas de Desarrollo

Al desarrollar o modificar código en este repositorio, debes cumplir las siguientes reglas arquitectónicas:

1. **Separación Estricta de Núcleos:**
   * **Cortex-M7 (CPU1):** Orquestador general, networking (FreeRTOS + LwIP Ethernet), interfaz de usuario/telemetría y DSP pesado.
   * **Cortex-M4 (CPU2):** Determinismo estricto, **Bare-Metal (sin RTOS)** para bucles de control rápido y adquisición ADC/DMA.
2. **Capas de Drivers en `01_shared_drivers/`:**
   * `*_hal.[c|h]`: Implementaciones de alto nivel ST HAL.
   * `*_ll.[c|h]`: Envoltorios de bajo nivel ST LL orientados a rendimiento.
   * `*_bm.[c|h]`: Registros puros Bare-Metal sin overhead de librerías.
3. **Memoria Compartida y D-Cache:**
   * Los buffers de intercambio inter-core (SRAM4 / AXI SRAM) y descriptores DMA deben estar alineados a **32 bytes** (`ALIGN_32BYTES`) y gestionados con invalidación/limpieza de caché (`SCB_CleanDCache`, `SCB_InvalidateDCache`) según las guías en `00_docs_stm32h7/docs/00_core_and_system/cache_mpu_rules.md`.
4. **Sincronización Inter-Core:**
   * Todo arranque y paso de mensajes entre núcleos se sincroniza mediante **Semáforos de Hardware (HSEM)**. CM7 inicia primero, enciende relojes y despierta al CM4 liberando `HSEM 0`.

---

## 7. Flujo de Trabajo en Git (Branching & Commits)

Para mantener la estabilidad del repositorio:

### Estrategia de Ramas
* **`main`:** Código estable y probado en hardware. Protegido contra commits directos.
* **`feature/<nombre>`:** Nuevas funcionalidades (ej. `feature/spi-dma-driver`).
* **`fix/<nombre>`:** Corrección de fallos (ej. `fix/adc-calibration-offset`).
* **`docs/<nombre>`:** Actualizaciones de documentación técnica.

### Convención de Commits (Conventional Commits)
Usa mensajes claros, en minúsculas y con prefijos de tipo:
* `feat(adc): add oversampling configuration support`
* `fix(eth): handle link-down reconnection in lwip task`
* `refactor(hsem): unify lock timeout handling`
* `docs(readme): update nucleo jumper wiring table`

### Creación de Pull Requests (PR)
1. Actualiza tu rama con respecto a `main` antes de enviar (`git pull origin main --rebase`).
2. Verifica que el proyecto compila limpio (`cmake --build build`).
3. Describe en la PR:
   - Resumen de los cambios.
   - Proyectos de firmware impactados.
   - Pruebas realizadas en hardware (si aplica).

---

## 8. Resolución de Problemas Frecuentes (FAQ)

### ❓ `Fatal error: STM32CubeH7 is missing or incomplete`
* **Causa:** No se ha ejecutado el script de restauración del SDK.
* **Solución:** Ejecuta `.\tools\init_cube_submodule.ps1` desde la raíz del workspace.

### ❓ `Filename too long` en Git para Windows
* **Causa:** Rutas profundas de ST superan los 260 caracteres de la API Win32 antigua.
* **Solución:** Ejecuta `git config --system core.longpaths true` con permisos de administrador.

### ❓ `arm-none-eabi-gcc: command not found`
* **Causa:** El compilador cruzado no está en las variables de entorno.
* **Solución:** Ejecuta `.\tools\env.ps1` en PowerShell o añade manualmente la carpeta `bin` de tu toolchain al `PATH`.

### ❓ ST-Link no detecta la placa o el target no responde
* **Causa:** Placa en estado de bajo consumo o debugger bloqueado en dual-core.
* **Solución:** Mantén presionado el botón negro de **RESET** en la placa Nucleo, ejecuta `python tools/flash.py` y suelta el botón cuando comience la conexión.

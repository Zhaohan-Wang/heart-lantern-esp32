#!/bin/zsh
# 带实时进度心跳的 ESP-IDF 构建脚本
# 用法:
#   zsh scripts/build_with_progress.zsh          # 只构建
#   zsh scripts/build_with_progress.zsh --flash   # 构建 + 烧录
#
# 另开终端实时看日志:
#   tail -f /tmp/heart_lantern_build.log

set -e
set -o pipefail

SCRIPT_DIR=${0:A:h}
PROJECT=${PROJECT:-${SCRIPT_DIR:h}}
IDF=${IDF_PATH:-$HOME/esp/v5.4.1/esp-idf}
IDF_TOOLS=${IDF_TOOLS_PATH:-$HOME/.espressif}
IDF_CLI="$IDF/tools/idf.py"
LOG=/tmp/heart_lantern_build.log
BUILD_FLAG=/tmp/heart_lantern_build_building
HEARTBEAT_SEC=8
DO_FLASH=false

if [[ "${1:-}" == "--flash" ]]; then
  DO_FLASH=true
fi

find_idf_python() {
  if [[ -n "${IDF_PYTHON_ENV_PATH:-}" && -x "$IDF_PYTHON_ENV_PATH/bin/python" ]]; then
    echo "$IDF_PYTHON_ENV_PATH/bin/python"
    return 0
  fi

  local candidates=("$IDF_TOOLS"/python_env/idf5.4_py*_env/bin/python(N))
  if (( ${#candidates} > 0 )); then
    echo "${candidates[-1]}"
    return 0
  fi

  return 1
}

progress() {
  echo ""
  echo ">>> $(date '+%H:%M:%S') $*"
  echo ""
}

# 用 pgrep 查活跃进程，避免 grep -E 把 g++ 里的 ++ 当成正则
get_active_procs() {
  pgrep -fl 'cmake|ninja|idf_component|idf\.py|xtensa-esp32s3' 2>/dev/null \
    | awk '{for(i=2;i<=NF;i++) printf "%s ", $i; print ""}' \
    | sort -u \
    | tr '\n' ' '
}

# 后台心跳：构建进行中每隔几秒打印状态
start_heartbeat() {
  (
    local start=$SECONDS
    while [[ -f "$BUILD_FLAG" ]]; do
      local elapsed=$(( SECONDS - start ))
      local obj_count=0
      local stage="等待 idf.py 输出"

      if [[ -d "$PROJECT/build" ]]; then
        obj_count=$(find "$PROJECT/build" -name '*.obj' 2>/dev/null | wc -l | tr -d ' ')
        if [[ -f "$PROJECT/build/build.ninja" ]]; then
          stage="CMake 已完成，Ninja 编译中"
        elif [[ -f "$PROJECT/build/CMakeCache.txt" ]]; then
          stage="CMake 配置中（这步可能 3~10 分钟无新行，属正常）"
        else
          stage="CMake 初始化中"
        fi
      fi

      local active_procs
      active_procs=$(get_active_procs)
      [[ -z "$active_procs" ]] && active_procs="(暂无子进程：可能在 idf_component_manager.idf_extensions 导入，x86 Anaconda venv 下可需 5~15 分钟)"

      local last_line="(尚无输出)"
      if [[ -s "$LOG" ]]; then
        last_line=$(tail -1 "$LOG" 2>/dev/null | cut -c1-120)
      fi

      echo ""
      echo "━━━ 心跳 ${elapsed}s ━━━ 阶段: ${stage}"
      echo "    已编译 .obj 数量: ${obj_count}"
      echo "    活跃进程: ${active_procs}"
      echo "    日志最后一行: ${last_line}"
      echo "    完整日志: tail -f ${LOG}"
      echo ""

      sleep "$HEARTBEAT_SEC"
    done
  ) &
  HEARTBEAT_PID=$!
}

stop_heartbeat() {
  rm -f "$BUILD_FLAG"
  if [[ -n "${HEARTBEAT_PID:-}" ]]; then
    kill "$HEARTBEAT_PID" 2>/dev/null || true
    wait "$HEARTBEAT_PID" 2>/dev/null || true
  fi
}

trap stop_heartbeat EXIT

progress "[0/6] 清理残留进程"
pkill -9 -f "esp-idf/tools/idf.py" 2>/dev/null || true
pkill -9 -f "hardware-oled-keyboard.*cmake" 2>/dev/null || true
pkill -9 -f "idf_component_manager" 2>/dev/null || true
pkill -9 -f "activate.py --export" 2>/dev/null || true
sleep 1

progress "[1/6] 检查工具链"
if [[ ! -f "$IDF_CLI" ]]; then
  echo "ERROR: ESP-IDF not found at $IDF"
  echo "Install it first, or set IDF_PATH=/path/to/esp-idf."
  exit 1
fi

IDF_PY=$(find_idf_python) || {
  echo "ERROR: ESP-IDF Python environment not found under $IDF_TOOLS/python_env"
  echo "Run: cd $IDF && ./install.sh esp32s3"
  exit 1
}

$IDF_PY -c "import serial; print('idf python serial OK')"
$IDF_PY -m esptool version
test -x "$IDF_TOOLS/tools/xtensa-esp-elf/esp-14.2.0_20241119/xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"

progress "[2/6] 加载 ESP-IDF 环境（仅 export，跳过 zsh 补全）"
unset PYTHONPATH PYTHONHOME
export CONDA_AUTO_ACTIVATE_BASE=false
export CONDA_SHLVL=0
export IDF_PATH="$IDF"
export IDF_PYTHON_ENV_PATH="${IDF_PY:h:h}"
export PYTHONUNBUFFERED=1
ENV_CACHE="$IDF_TOOLS/heart_lantern_idf_env.zsh"
if [[ -s "$ENV_CACHE" ]]; then
  echo "使用缓存环境: $ENV_CACHE"
  source "$ENV_CACHE"
else
  echo "导出 ESP-IDF 环境（activate.py）..."
  _ACTIVATE_FILE=$($IDF_PY "$IDF/tools/activate.py" --export --shell zsh -q | sed -n 's/^\. //p')
  eval "$(grep '^export ' "$_ACTIVATE_FILE")"
  unset _ACTIVATE_FILE
fi
echo "IDF_PATH=$IDF_PATH"
echo "ESP_ROM_ELF_DIR=${ESP_ROM_ELF_DIR:-未设置}"
$IDF_PY -c "import idf_component_manager; print('idf_component_manager OK')"

progress "[3/6] 开始构建（日志写入 ${LOG}）"
echo "提示: 另开终端执行  tail -f ${LOG}  可实时看每一行输出"
echo "提示: CMake 配置阶段可能长时间无新行，请看上方「心跳」确认还在跑"
echo ""

cd "$PROJECT"
: > "$LOG"
touch "$BUILD_FLAG"
start_heartbeat

# 前台构建 + tee；pipefail 确保 idf.py 失败时脚本能捕获
$IDF_PY "$IDF_CLI" -v build 2>&1 | tee -a "$LOG"
BUILD_EXIT=${pipestatus[1]:-$?}

stop_heartbeat

if [[ "$BUILD_EXIT" -ne 0 ]]; then
  echo ""
  echo "ERROR: 构建失败，退出码 ${BUILD_EXIT}"
  echo "查看日志: tail -30 ${LOG}"
  tail -30 "$LOG" 2>/dev/null || true
  exit "$BUILD_EXIT"
fi

if [[ ! -f build/build.ninja && ! -f build/Makefile ]]; then
  echo "ERROR: build/build.ninja 和 build/Makefile 都不存在，构建未真正完成"
  exit 1
fi

progress "[4/6] 验证 main.c"
if [[ -f build/compile_commands.json ]]; then
  BUILD_GRAPH=build/compile_commands.json
elif [[ -f build/build.ninja ]]; then
  BUILD_GRAPH=build/build.ninja
else
  BUILD_GRAPH=build/Makefile
fi

if ! grep -q "main.c" "$BUILD_GRAPH"; then
  echo "ERROR: $BUILD_GRAPH 里没有 main.c"
  exit 1
fi
echo "OK: main.c 已在构建图中"
ls -la build/heart_lantern.bin

if [[ "$DO_FLASH" == true ]]; then
  progress "[5/6] 烧录"
  PORT="${ESPPORT:-}"
  if [[ ! -e "$PORT" ]]; then
    PORT=$(find /dev -maxdepth 1 \( -name 'cu.usbserial*' -o -name 'cu.usbmodem*' \) 2>/dev/null | sort | tail -1)
  fi
  if [[ -z "$PORT" ]] || [[ ! -e "$PORT" ]]; then
    echo "ERROR: 未找到串口（可 export ESPPORT=/dev/cu.xxx）"
    exit 1
  fi
  echo "PORT=$PORT"
  # 用 idf.py flash，避免 esptool v4/v5 参数名差异
  $IDF_PY "$IDF_CLI" -p "$PORT" flash
  progress "[6/6] 烧录完成 → $PORT"
else
  progress "[5/6] 跳过烧录（加 --flash 参数可自动烧录）"
fi

progress "全部完成"

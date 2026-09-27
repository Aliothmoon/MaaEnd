# Android 客户端

外壳是 [MaaFwApp](https://github.com/Aliothmoon/MaaFwApp) 子模块。资源和两个编译型 agent 用本仓库的树。

MaaFwApp 只认 `agent.sourceDir/<abi>/jniLibs/lib*.so`，构建期由 `syncAgentJniLibs` 铺进 APK 的 `lib/<abi>/`。框架 `.so` 不走这条，由外壳自己的 `setup_maa_framework.py` 铺。

## 首次

```bash
git submodule update --init --recursive
python tools/build_android_agents.py
python Android/MaaFwApp/scripts/setup_maa_framework.py --abi arm64-v8a --tag v5.12.3
```

在 `Android/MaaFwApp/local.properties` 里写（不进 git）：

```properties
sdk.dir=<Android SDK>
pi.profile=../profile.yaml
build.debugAbi=arm64-v8a
```

Windows 上 NDK 认 `ANDROID_NDK_ROOT`（或 `ANDROID_HOME/ndk` 里最新一份）。CMake 要 ≥ 3.28，Android SDK 自带的 3.31+ 即可。Go 交叉编必须 `CGO_ENABLED=1`（`purego` 在 Android 上要 cgo 才能 `dlopen`）。

## 出包

```bash
# 改了 go-service / cpp-algo
python tools/build_android_agents.py

# 已连接设备
./Android/MaaFwApp/gradlew -p Android/MaaFwApp :app:installDebug
```

只改 `assets/` 里的任务 / 图，重新 `installDebug` 即可。

升外壳：

```bash
git -C Android/MaaFwApp fetch
git -C Android/MaaFwApp checkout origin/main
git add Android/MaaFwApp
```

package control

import "testing"

func TestControlTypeFromInfo(t *testing.T) {
	cases := []struct {
		name string
		info string
		want string
	}{
		{"win32", `{"type":"win32","hwnd":1}`, CONTROL_TYPE_WIN32},
		{"adb", `{"type":"adb","adb_serial":"127.0.0.1:5555"}`, CONTROL_TYPE_ADB},
		// MaaFwApp 在手机上用的 Android 原生控制器：触控 + 移动端 UI，按 ADB 处理
		{"native android", `{"type":"native_android","display_id":0,"touch_resolution":{"width":1280,"height":720}}`, CONTROL_TYPE_ADB},
		{"native android fallback", `type=native_android`, CONTROL_TYPE_ADB},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			got, err := controlTypeFromInfo(c.info)
			if err != nil {
				t.Fatalf("controlTypeFromInfo(%q) error: %v", c.info, err)
			}
			if got != c.want {
				t.Fatalf("controlTypeFromInfo(%q) = %q, want %q", c.info, got, c.want)
			}
		})
	}
}

func TestControlTypeFromInfoRejectsUnknown(t *testing.T) {
	for _, info := range []string{"", `{"type":"dbg"}`, `{}`} {
		if got, err := controlTypeFromInfo(info); err == nil {
			t.Fatalf("controlTypeFromInfo(%q) = %q, want error", info, got)
		}
	}
}

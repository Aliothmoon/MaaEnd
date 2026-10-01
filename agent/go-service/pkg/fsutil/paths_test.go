package fsutil

import (
	"path/filepath"
	"testing"
)

func TestOutputPath(t *testing.T) {
	root, err := StartupDir()
	if err != nil {
		t.Fatal(err)
	}
	if !filepath.IsAbs(root) {
		t.Fatalf("startup directory is not absolute: %q", root)
	}
	if got := OutputPath(); got != root {
		t.Fatalf("OutputPath() = %q, want %q", got, root)
	}
	want := filepath.Join(root, "debug", "record", "IMS.json")
	if got := OutputPath("debug", "record", "IMS.json"); got != want {
		t.Fatalf("OutputPath() = %q, want %q", got, want)
	}
}

func TestOutputPathIgnoresChdir(t *testing.T) {
	root, err := StartupDir()
	if err != nil {
		t.Fatal(err)
	}
	before := OutputPath("debug", "go-service.log")
	t.Chdir(t.TempDir())
	if got := OutputPath("debug", "go-service.log"); got != before {
		t.Fatalf("output path changed after chdir: %q, want %q", got, before)
	}
	if got, err := StartupDir(); err != nil || got != root {
		t.Fatalf("StartupDir() after chdir = %q, %v; want %q, nil", got, err, root)
	}
}

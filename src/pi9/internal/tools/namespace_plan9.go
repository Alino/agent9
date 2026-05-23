//go:build plan9
// +build plan9

package tools

import (
	"encoding/json"
	"fmt"
	"os"

	"golang.org/x/sys/plan9"
)

// flagFromName converts a user-friendly flag name to plan9 syscall flag.
// Default: MREPL (replace).
func flagFromName(name string) (int, error) {
	switch name {
	case "", "replace":
		return plan9.MREPL, nil
	case "before":
		return plan9.MBEFORE, nil
	case "after":
		return plan9.MAFTER, nil
	case "create":
		return plan9.MREPL | plan9.MCREATE, nil
	default:
		return 0, fmt.Errorf("unknown flag %q (want: replace, before, after, create)", name)
	}
}

// bindTool wraps the bind(2) syscall. Effects pi9's own namespace only.
func bindTool(argsJSON string) (string, error) {
	var args struct {
		Src  string `json:"src"`
		Dst  string `json:"dst"`
		Flag string `json:"flag"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Src == "" || args.Dst == "" {
		return "", fmt.Errorf("src and dst are required")
	}
	flag, err := flagFromName(args.Flag)
	if err != nil {
		return "", err
	}
	if err := plan9.Bind(args.Src, args.Dst, flag); err != nil {
		return "", err
	}
	return fmt.Sprintf("bound %s onto %s (flag=%s)", args.Src, args.Dst, args.Flag), nil
}

// mountTool wraps the mount(2) syscall. The first arg is a path to a
// /srv file (e.g. /srv/cfs) — we open it, pass the fd to mount, and
// affix it at mountpoint. afd = -1 (no auth file) which matches what
// /bin/mount does without -a.
func mountTool(argsJSON string) (string, error) {
	var args struct {
		Srv        string `json:"srv"`
		Mountpoint string `json:"mountpoint"`
		Flag       string `json:"flag"`
	}
	if err := json.Unmarshal([]byte(argsJSON), &args); err != nil {
		return "", fmt.Errorf("bad json: %w", err)
	}
	if args.Srv == "" || args.Mountpoint == "" {
		return "", fmt.Errorf("srv and mountpoint are required")
	}
	flag, err := flagFromName(args.Flag)
	if err != nil {
		return "", err
	}
	f, err := os.OpenFile(args.Srv, os.O_RDWR, 0)
	if err != nil {
		return "", fmt.Errorf("open %s: %w", args.Srv, err)
	}
	defer f.Close()
	if err := plan9.Mount(int(f.Fd()), -1, args.Mountpoint, flag, ""); err != nil {
		return "", err
	}
	return fmt.Sprintf("mounted %s on %s (flag=%s)", args.Srv, args.Mountpoint, args.Flag), nil
}

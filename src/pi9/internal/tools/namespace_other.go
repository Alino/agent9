//go:build !plan9
// +build !plan9

package tools

import "fmt"

// bindTool is plan9-only; on other OSes return a clean error.
func bindTool(argsJSON string) (string, error) {
	return "", fmt.Errorf("bind is Plan 9 only")
}

// mountTool is plan9-only.
func mountTool(argsJSON string) (string, error) {
	return "", fmt.Errorf("mount is Plan 9 only")
}

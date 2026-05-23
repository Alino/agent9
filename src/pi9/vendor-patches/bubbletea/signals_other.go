//go:build !plan9
// +build !plan9

package tea

// vtsListenForResize is a no-op on non-plan9 platforms. The plan9
// build implements this in signals_plan9.go to hook vts's per-session
// resize notifications. See handleResize() in tea.go for the call
// site.
func vtsListenForResize(p *Program, done chan struct{}) bool {
	return false
}

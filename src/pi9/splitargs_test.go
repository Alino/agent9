package main

import (
	"reflect"
	"testing"
)

func TestSplitArgs(t *testing.T) {
	cases := []struct {
		in   string
		want []string
	}{
		{"/model", []string{"/model"}},
		{"/model gpt-5", []string{"/model", "gpt-5"}},
		{`/review "foo bar" baz`, []string{"/review", "foo bar", "baz"}},
		{"/x 'single quoted'", []string{"/x", "single quoted"}},
		{`/x "a \"b\" c"`, []string{"/x", `a "b" c`}},
		{"   /spaced   arg  ", []string{"/spaced", "arg"}},
		{"", nil},
	}
	for _, c := range cases {
		got := splitArgs(c.in)
		if !reflect.DeepEqual(got, c.want) {
			t.Errorf("splitArgs(%q) = %#v, want %#v", c.in, got, c.want)
		}
	}
}

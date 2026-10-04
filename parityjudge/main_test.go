package main

import (
	"bytes"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

type failOnRead struct {
	reads int
}

func TestNormalizeReferenceHTMLFonts(t *testing.T) {
	face := func(family, payload, weight, style string) string {
		return `@font-face{font-family:"` + family + `";src:url(data:font/woff2;base64,` + payload +
			") format('woff2');font-weight:" + weight + ";font-style:" + style + ";font-display:block;}\n"
	}
	normal := face("JetBrains Mono", "AQID", "400", "normal")
	bold := face("JetBrains Mono", "AQID", "700", "normal")
	italic := face("JetBrains Mono", "BAUG", "400", "italic")
	boldItalic := face("JetBrains Mono", "BAUG", "700", "italic")
	weightRange := face("JetBrains Mono", "AQID", "400 700", "normal")
	italicRange := face("JetBrains Mono", "BAUG", "400 700", "italic")
	cases := []struct{ name, input, want string }{
		{"paired styles", "<style>\n" + normal + bold + italic + boldItalic + "</style>\nbody\n",
			"<style>\n" + weightRange + italicRange + "</style>\nbody\n"},
		{"already deduplicated", weightRange + italicRange, weightRange + italicRange},
		{"different bytes", normal + face("JetBrains Mono", "BAUG", "700", "normal"),
			normal + face("JetBrains Mono", "BAUG", "700", "normal")},
		{"different styles", normal + boldItalic, normal + boldItalic},
		{"reversed weights", bold + normal, bold + normal},
		{"nonadjacent", normal + "/* separator */\n" + bold, normal + "/* separator */\n" + bold},
		{"custom family", face("Custom", "AQID", "400", "normal") + face("Custom", "AQID", "700", "normal"),
			face("Custom", "AQID", "400", "normal") + face("Custom", "AQID", "700", "normal")},
		{"no fonts", "<style>\nbody{}\n</style>\nbody\n", "<style>\nbody{}\n</style>\nbody\n"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if got := string(normalizeReferenceHTMLFonts([]byte(tc.input))); got != tc.want {
				t.Fatalf("font normalization = %q, want %q", got, tc.want)
			}
			if got := string(normalizeReferenceHTMLFonts([]byte(tc.want))); got != tc.want {
				t.Fatalf("font normalization is not idempotent: %q", got)
			}
		})
	}
}

func (r *failOnRead) Read([]byte) (int, error) {
	r.reads++
	return 0, errors.New("suite dispatch must not read stdin")
}

func TestSuiteCommandInputDoesNotReadStdin(t *testing.T) {
	stdin := &failOnRead{}
	data, err := commandInput(stdin, true, "testdata/table-corpus")
	if err != nil {
		t.Fatalf("suite command input: %v", err)
	}
	if data != nil {
		t.Fatalf("suite command input = %q, want nil", data)
	}
	if stdin.reads != 0 {
		t.Fatalf("suite command read stdin %d time(s)", stdin.reads)
	}
}

func TestNonSuiteCommandInputReadsStdin(t *testing.T) {
	data, err := commandInput(bytes.NewBufferString("# input\n"), true, "")
	if err != nil {
		t.Fatalf("single comparison input: %v", err)
	}
	if got, want := string(data), "# input\n"; got != want {
		t.Fatalf("single comparison input = %q, want %q", got, want)
	}
}

func TestParseCaseExcludeList(t *testing.T) {
	excludes, err := parseCaseExcludeList("/tmp/a.md@20, /tmp/b.md@40")
	if err != nil {
		t.Fatalf("parse case exclusions: %v", err)
	}
	if !caseExcluded("/tmp/a.md", 20, excludes) || !caseExcluded("/tmp/b.md", 40, excludes) {
		t.Fatalf("expected exclusions missing: %#v", excludes)
	}
	if caseExcluded("/tmp/a.md", 40, excludes) {
		t.Fatal("case exclusion must not suppress other widths")
	}
	if _, err := parseCaseExcludeList("/tmp/a.md"); err == nil {
		t.Fatal("malformed case exclusion must fail")
	}
}

func TestSuiteRejectsExcludingEveryCase(t *testing.T) {
	suite := t.TempDir()
	fixture := filepath.Join(suite, "fixture.md")
	if err := os.WriteFile(fixture, []byte("# fixture\n"), 0o600); err != nil {
		t.Fatalf("write fixture: %v", err)
	}
	err := compareLibmdfSuite("ansi", suite, nil,
		map[parityCaseExclude]struct{}{{path: fixture, width: 80}: {}},
		[]int{4096}, []int{80}, []parityOptions{defaultParityOptions()}, false, 1)
	if err == nil {
		t.Fatal("suite with every case excluded must fail")
	}
	if !strings.Contains(err.Error(), "no parity cases remain after exclusions") {
		t.Fatalf("unexpected all-excluded error: %v", err)
	}
}

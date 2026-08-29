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

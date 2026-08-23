package main

import (
	"bytes"
	"errors"
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

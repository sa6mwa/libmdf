package main

import (
	"bytes"
	"flag"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"sort"
	"strconv"
	"strings"
	"sync"

	"pkt.systems/mdf"
	mdfhtml "pkt.systems/mdf/html"
)

type chunkReader struct {
	data  []byte
	off   int
	chunk int
}

func (r *chunkReader) Read(p []byte) (int, error) {
	if r.off >= len(r.data) {
		return 0, io.EOF
	}
	n := r.chunk
	if n <= 0 || n > len(p) {
		n = len(p)
	}
	if remain := len(r.data) - r.off; n > remain {
		n = remain
	}
	copy(p, r.data[r.off:r.off+n])
	r.off += n
	return n, nil
}

type tokenDump struct {
	w io.Writer
}

type parityOptions struct {
	theme       string
	boring      bool
	osc8        bool
	tableBuffer mdf.TableBufferMode
	tableWire   mdf.TableWireMode
}

func defaultParityOptions() parityOptions {
	return parityOptions{
		theme:       "default",
		osc8:        false,
		tableBuffer: mdf.TableBufferFull,
		tableWire:   mdf.TableWireLine,
	}
}

func esc(s string) string {
	s = strings.ReplaceAll(s, "\\", "\\\\")
	s = strings.ReplaceAll(s, "\n", "\\n")
	s = strings.ReplaceAll(s, "\r", "\\r")
	s = strings.ReplaceAll(s, "\t", "\\t")
	return s
}

func (d tokenDump) WriteToken(tok mdf.StreamToken) error {
	_, err := fmt.Fprintf(d.w, "token\t%d\t%s\t%s\t%s\t%t\n", tok.Kind, esc(tok.Text), esc(tok.Style.Prefix), esc(tok.LinkURL), tok.CodeBlock)
	return err
}

func (d tokenDump) Flush() error         { return nil }
func (d tokenDump) Width() int           { return 80 }
func (d tokenDump) SetWidth(int)         {}
func (d tokenDump) SetWrapIndent(string) {}

func openTraceEncoder(path string) (mdf.WriteTraceEncoder, io.Closer, error) {
	path = strings.TrimSpace(path)
	if path == "" {
		return nil, nil, nil
	}
	if path == "-" {
		return mdf.NewNDJSONWriteTraceEncoder(os.Stderr), nil, nil
	}
	f, err := os.Create(path)
	if err != nil {
		return nil, nil, err
	}
	return mdf.NewNDJSONWriteTraceEncoder(f), f, nil
}

func themeByName(name string, boring bool) (mdf.Theme, error) {
	if boring {
		return mdf.NewTheme("boring", mdf.Styles{}), nil
	}
	if name == "" {
		name = "default"
	}
	theme, ok := mdf.ThemeByName(name)
	if !ok {
		return nil, fmt.Errorf("unknown theme %q", name)
	}
	return theme, nil
}

func renderOptions(opts parityOptions, traceEncoder mdf.WriteTraceEncoder) []mdf.RenderOption {
	out := []mdf.RenderOption{
		mdf.WithOSC8(opts.osc8),
		mdf.WithTableBufferMode(opts.tableBuffer),
		mdf.WithTableWireMode(opts.tableWire),
	}
	if traceEncoder != nil {
		out = append(out, mdf.WithWriteTrace(traceEncoder))
	}
	return out
}

func render(mode string, data []byte, chunk int, width int, opts parityOptions, tracePath string) ([]byte, error) {
	var out bytes.Buffer
	var writer io.Writer
	traceEncoder, traceCloser, err := openTraceEncoder(tracePath)
	if err != nil {
		return nil, err
	}
	if traceCloser != nil {
		defer traceCloser.Close()
	}
	r := &chunkReader{data: data, chunk: chunk}
	writer = &out
	switch mode {
	case "ansi":
		theme, err := themeByName(opts.theme, opts.boring)
		if err != nil {
			return nil, err
		}
		err = mdf.Render(mdf.RenderRequest{
			Reader:  r,
			Writer:  writer,
			Width:   width,
			Theme:   theme,
			Options: renderOptions(opts, traceEncoder),
		})
		return out.Bytes(), err
	case "html":
		if traceEncoder != nil {
			return nil, fmt.Errorf("--trace-writes is only supported for ANSI output")
		}
		theme, err := themeByName(opts.theme, false)
		if err != nil {
			return nil, err
		}
		cfg := mdfhtml.DefaultConfig()
		cfg.Boring = opts.boring
		cfg.TableBufferMode = opts.tableBuffer
		cfg.TableWireMode = opts.tableWire
		err = mdfhtml.Render(mdfhtml.RenderRequest{Reader: r, Writer: writer, Theme: theme, Config: cfg})
		return out.Bytes(), err
	case "tokens":
		theme, err := themeByName(opts.theme, opts.boring)
		if err != nil {
			return nil, err
		}
		err = mdf.Parse(mdf.ParseRequest{
			Reader:  r,
			Stream:  tokenDump{w: &out},
			Theme:   theme,
			Options: renderOptions(opts, nil),
		})
		return out.Bytes(), err
	default:
		return nil, fmt.Errorf("unknown mode %q", mode)
	}
}

func renderReference(mode string, data []byte, chunk int, width int, opts parityOptions, trace bool) ([]byte, []byte, error) {
	var out bytes.Buffer
	var traceBuf bytes.Buffer
	var writer io.Writer
	var traceEncoder mdf.WriteTraceEncoder

	writer = &out
	if trace {
		if mode != "ansi" {
			return nil, nil, fmt.Errorf("write trace comparison is only supported for ANSI output")
		}
		traceEncoder = mdf.NewNDJSONWriteTraceEncoder(&traceBuf)
	}
	switch mode {
	case "ansi":
		theme, err := themeByName(opts.theme, opts.boring)
		if err != nil {
			return nil, nil, err
		}
		err = mdf.Render(mdf.RenderRequest{
			Reader:  &chunkReader{data: data, chunk: chunk},
			Writer:  writer,
			Width:   width,
			Theme:   theme,
			Options: renderOptions(opts, traceEncoder),
		})
		return out.Bytes(), traceBuf.Bytes(), err
	case "html":
		theme, err := themeByName(opts.theme, false)
		if err != nil {
			return nil, nil, err
		}
		cfg := mdfhtml.DefaultConfig()
		cfg.Boring = opts.boring
		cfg.TableBufferMode = opts.tableBuffer
		cfg.TableWireMode = opts.tableWire
		err = mdfhtml.Render(mdfhtml.RenderRequest{Reader: &chunkReader{data: data, chunk: chunk}, Writer: writer, Theme: theme, Config: cfg})
		return out.Bytes(), traceBuf.Bytes(), err
	default:
		return nil, nil, fmt.Errorf("libmdf comparison is not supported for mode %q", mode)
	}
}

func parseIntList(raw string, env string, fallback string) ([]int, error) {
	var out []int
	if raw == "" {
		raw = os.Getenv(env)
	}
	if raw == "" {
		raw = fallback
	}
	for _, part := range strings.FieldsFunc(raw, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' || r == '\n' }) {
		if part == "" {
			continue
		}
		n, err := strconv.Atoi(part)
		if err != nil {
			return nil, fmt.Errorf("parse %s value %q: %w", env, part, err)
		}
		out = append(out, n)
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no values for %s", env)
	}
	return out, nil
}

func parseBoolList(raw string, env string, fallback string) ([]bool, error) {
	var out []bool
	if raw == "" {
		raw = os.Getenv(env)
	}
	if raw == "" {
		raw = fallback
	}
	for _, part := range strings.FieldsFunc(raw, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' || r == '\n' }) {
		switch strings.ToLower(strings.TrimSpace(part)) {
		case "", "skip":
			continue
		case "1", "true", "on", "yes":
			out = append(out, true)
		case "0", "false", "off", "no":
			out = append(out, false)
		default:
			return nil, fmt.Errorf("parse %s value %q", env, part)
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no values for %s", env)
	}
	return out, nil
}

func parseThemeList(raw string) ([]string, error) {
	if raw == "" {
		raw = os.Getenv("LIBMDF_PARITY_THEMES")
	}
	if raw == "" {
		raw = "default"
	}
	if strings.EqualFold(strings.TrimSpace(raw), "all") {
		return mdf.AvailableThemes(), nil
	}
	var out []string
	for _, part := range strings.FieldsFunc(raw, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' || r == '\n' }) {
		name := strings.TrimSpace(part)
		if name == "" {
			continue
		}
		if _, ok := mdf.ThemeByName(name); !ok {
			return nil, fmt.Errorf("unknown theme %q", name)
		}
		out = append(out, strings.ToLower(name))
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no values for LIBMDF_PARITY_THEMES")
	}
	sort.Strings(out)
	return out, nil
}

func parseTableBufferList(raw string) ([]mdf.TableBufferMode, error) {
	if raw == "" {
		raw = os.Getenv("LIBMDF_PARITY_TABLE_BUFFERS")
	}
	if raw == "" {
		raw = "full"
	}
	var out []mdf.TableBufferMode
	for _, part := range strings.FieldsFunc(raw, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' || r == '\n' }) {
		switch strings.ToLower(strings.TrimSpace(part)) {
		case "", "skip":
			continue
		case "full":
			out = append(out, mdf.TableBufferFull)
		case "row":
			out = append(out, mdf.TableBufferRow)
		default:
			return nil, fmt.Errorf("unknown table buffer mode %q", part)
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no values for LIBMDF_PARITY_TABLE_BUFFERS")
	}
	return out, nil
}

func parseTableWireList(raw string) ([]mdf.TableWireMode, error) {
	if raw == "" {
		raw = os.Getenv("LIBMDF_PARITY_TABLE_WIRES")
	}
	if raw == "" {
		raw = "line"
	}
	var out []mdf.TableWireMode
	for _, part := range strings.FieldsFunc(raw, func(r rune) bool { return r == ',' || r == ' ' || r == '\t' || r == '\n' }) {
		switch strings.ToLower(strings.TrimSpace(part)) {
		case "", "skip":
			continue
		case "line":
			out = append(out, mdf.TableWireLine)
		case "ascii":
			out = append(out, mdf.TableWireASCII)
		case "space":
			out = append(out, mdf.TableWireSpace)
		default:
			return nil, fmt.Errorf("unknown table wire mode %q", part)
		}
	}
	if len(out) == 0 {
		return nil, fmt.Errorf("no values for LIBMDF_PARITY_TABLE_WIRES")
	}
	return out, nil
}

func tableBufferName(mode mdf.TableBufferMode) string {
	switch mode {
	case mdf.TableBufferFull:
		return "full"
	case mdf.TableBufferRow:
		return "row"
	default:
		return fmt.Sprintf("unknown-%d", mode)
	}
}

func tableWireName(mode mdf.TableWireMode) string {
	switch mode {
	case mdf.TableWireLine:
		return "line"
	case mdf.TableWireASCII:
		return "ascii"
	case mdf.TableWireSpace:
		return "space"
	default:
		return fmt.Sprintf("unknown-%d", mode)
	}
}

func parityOptionSets(themes []string, borings []bool, osc8s []bool, buffers []mdf.TableBufferMode, wires []mdf.TableWireMode) []parityOptions {
	var out []parityOptions
	for _, theme := range themes {
		for _, boring := range borings {
			for _, osc8 := range osc8s {
				for _, buffer := range buffers {
					for _, wire := range wires {
						out = append(out, parityOptions{
							theme:       theme,
							boring:      boring,
							osc8:        osc8,
							tableBuffer: buffer,
							tableWire:   wire,
						})
					}
				}
			}
		}
	}
	return out
}

func collectMarkdownFiles(root string) ([]string, error) {
	var files []string
	err := filepath.WalkDir(root, func(path string, d os.DirEntry, err error) error {
		if err != nil {
			return err
		}
		if d.IsDir() {
			return nil
		}
		if filepath.Ext(path) == ".md" {
			files = append(files, path)
		}
		return nil
	})
	if err != nil {
		return nil, err
	}
	sort.Strings(files)
	return files, nil
}

func compareLibmdfOne(mode string, data []byte, chunk int, width int, opts parityOptions, trace bool) error {
	goOut, goTrace, err := renderReference(mode, data, chunk, width, opts, trace)
	if err != nil {
		return fmt.Errorf("reference render: %w", err)
	}
	cOut, cTrace, err := renderLibmdf(mode, data, chunk, width, opts, trace)
	if err != nil {
		return err
	}
	if !bytes.Equal(goOut, cOut) {
		return fmt.Errorf("output mismatch\n%s", firstDiffContext(goOut, cOut))
	}
	if trace {
		if !bytes.Equal(goTrace, cTrace) {
			return fmt.Errorf("trace mismatch\n%s", firstDiffContext(goTrace, cTrace))
		}
		return nil
	}
	return nil
}

func firstDiffContext(want []byte, got []byte) string {
	i := 0
	for i < len(want) && i < len(got) && want[i] == got[i] {
		i++
	}
	wantLine := lineAt(want, i)
	gotLine := lineAt(got, i)
	return fmt.Sprintf("first diff byte=%d\nwant: %s\ngot:  %s", i, quoteForDiff(wantLine), quoteForDiff(gotLine))
}

func lineAt(data []byte, pos int) []byte {
	if pos > len(data) {
		pos = len(data)
	}
	start := pos
	for start > 0 && data[start-1] != '\n' {
		start--
	}
	end := pos
	for end < len(data) && data[end] != '\n' {
		end++
	}
	return data[start:end]
}

func quoteForDiff(data []byte) string {
	if len(data) > 240 {
		data = data[:240]
	}
	return strconv.Quote(string(data))
}

type paritySuiteCase struct {
	md    string
	data  []byte
	chunk int
	width int
	opts  parityOptions
}

type paritySuiteCaseKey struct {
	md          string
	chunk       int
	width       int
	theme       string
	boring      bool
	osc8        bool
	tableBuffer string
	tableWire   string
}

func parseJobs(raw string) (int, error) {
	if raw == "" {
		raw = os.Getenv("LIBMDF_PARITY_JOBS")
	}
	if raw == "" {
		return runtime.GOMAXPROCS(0), nil
	}
	n, err := strconv.Atoi(raw)
	if err != nil {
		return 0, fmt.Errorf("parse LIBMDF_PARITY_JOBS value %q: %w", raw, err)
	}
	if n < 1 {
		return 0, fmt.Errorf("LIBMDF_PARITY_JOBS must be >= 1")
	}
	return n, nil
}

func markdownHasTableSyntax(data []byte) bool {
	lines := bytes.Split(data, []byte{'\n'})
	prevPipe := false
	for _, line := range lines {
		line = markdownStripContainerPrefix(line)
		if markdownLineHasTableSeparator(line) && prevPipe {
			return true
		}
		prevPipe = bytes.IndexByte(line, '|') >= 0
	}
	return false
}

func markdownStripContainerPrefix(line []byte) []byte {
	line = bytes.TrimLeft(line, " \t")
	for len(line) > 0 && line[0] == '>' {
		line = line[1:]
		line = bytes.TrimLeft(line, " \t")
	}
	return line
}

func markdownLineHasTableSeparator(line []byte) bool {
	if bytes.IndexByte(line, '|') < 0 {
		return false
	}
	fields := bytes.Split(line, []byte{'|'})
	sepFields := 0
	for _, field := range fields {
		if markdownTableSeparatorField(field) {
			sepFields++
		}
	}
	return sepFields >= 1
}

func markdownTableSeparatorField(field []byte) bool {
	field = bytes.TrimSpace(field)
	if len(field) == 0 {
		return false
	}
	if field[0] == ':' {
		field = field[1:]
	}
	if len(field) > 0 && field[len(field)-1] == ':' {
		field = field[:len(field)-1]
	}
	if len(field) < 3 {
		return false
	}
	for _, c := range field {
		if c != '-' {
			return false
		}
	}
	return true
}

func effectiveParityOptions(opts parityOptions, hasTable bool) parityOptions {
	if opts.boring {
		opts.theme = "default"
	}
	if !hasTable {
		opts.tableBuffer = mdf.TableBufferFull
		opts.tableWire = mdf.TableWireLine
	}
	return opts
}

func parityCaseKey(md string, chunk int, width int, opts parityOptions) paritySuiteCaseKey {
	return paritySuiteCaseKey{
		md:          md,
		chunk:       chunk,
		width:       width,
		theme:       opts.theme,
		boring:      opts.boring,
		osc8:        opts.osc8,
		tableBuffer: tableBufferName(opts.tableBuffer),
		tableWire:   tableWireName(opts.tableWire),
	}
}

func compareLibmdfSuiteCase(mode string, tc paritySuiteCase, trace bool) error {
	if err := compareLibmdfOne(mode, tc.data, tc.chunk, tc.width, tc.opts, trace); err != nil {
		if mode == "ansi" {
			return fmt.Errorf("mode=%s chunk=%d width=%d theme=%s boring=%t osc8=%t table-buffer=%s table-wire=%s file=%s: %w",
				mode,
				tc.chunk,
				tc.width,
				tc.opts.theme,
				tc.opts.boring,
				tc.opts.osc8,
				tableBufferName(tc.opts.tableBuffer),
				tableWireName(tc.opts.tableWire),
				tc.md,
				err)
		}
		return fmt.Errorf("mode=%s chunk=%d theme=%s boring=%t table-buffer=%s table-wire=%s file=%s: %w",
			mode,
			tc.chunk,
			tc.opts.theme,
			tc.opts.boring,
			tableBufferName(tc.opts.tableBuffer),
			tableWireName(tc.opts.tableWire),
			tc.md,
			err)
	}
	return nil
}

func compareLibmdfSuite(mode string, suite string, chunks []int, widths []int, optionSets []parityOptions, trace bool, jobs int) error {
	files, err := collectMarkdownFiles(suite)
	if err != nil {
		return err
	}
	if len(files) == 0 {
		return fmt.Errorf("no markdown fixtures under %s", suite)
	}
	var cases []paritySuiteCase
	seen := make(map[paritySuiteCaseKey]struct{})
	for _, md := range files {
		data, err := os.ReadFile(md)
		if err != nil {
			return err
		}
		hasTable := markdownHasTableSyntax(data)
		for _, chunk := range chunks {
			runWidths := widths
			if mode != "ansi" {
				runWidths = []int{0}
			}
			for _, width := range runWidths {
				for _, opts := range optionSets {
					if mode == "html" && opts.tableWire == mdf.TableWireASCII {
						continue
					}
					effective := effectiveParityOptions(opts, hasTable)
					key := parityCaseKey(md, chunk, width, effective)
					if _, ok := seen[key]; ok {
						continue
					}
					seen[key] = struct{}{}
					cases = append(cases, paritySuiteCase{
						md:    md,
						data:  data,
						chunk: chunk,
						width: width,
						opts:  effective,
					})
				}
			}
		}
	}
	if jobs < 1 {
		jobs = 1
	}
	if jobs > len(cases) {
		jobs = len(cases)
	}
	if jobs <= 1 {
		for _, tc := range cases {
			if err := compareLibmdfSuiteCase(mode, tc, trace); err != nil {
				return err
			}
		}
		return nil
	}
	var mu sync.Mutex
	next := 0
	var firstErr error
	var once sync.Once
	var wg sync.WaitGroup
	worker := func() {
		defer wg.Done()
		for {
			mu.Lock()
			if firstErr != nil || next >= len(cases) {
				mu.Unlock()
				return
			}
			tc := cases[next]
			next++
			mu.Unlock()
			if err := compareLibmdfSuiteCase(mode, tc, trace); err != nil {
				once.Do(func() {
					mu.Lock()
					firstErr = err
					mu.Unlock()
				})
				return
			}
		}
	}
	wg.Add(jobs)
	for i := 0; i < jobs; i++ {
		go worker()
	}
	wg.Wait()
	if firstErr != nil {
		return firstErr
	}
	return nil
}

func main() {
	mode := flag.String("mode", "ansi", "ansi|html|tokens")
	chunk := flag.Int("chunk", 4096, "reader chunk size")
	width := flag.Int("width", 80, "ansi width")
	traceWrites := flag.String("trace-writes", "", "write ANSI renderer-emission NDJSON trace to path, or - for stderr")
	compareLibmdf := flag.Bool("compare-libmdf", false, "compare Go mdf against libmdf through CGO")
	traceCompare := flag.Bool("trace-compare", false, "compare renderer-emission traces instead of final output")
	suite := flag.String("suite", "", "run comparison over markdown files under directory")
	chunksRaw := flag.String("chunks", "", "chunk sizes for --suite, separated by spaces or commas")
	widthsRaw := flag.String("widths", "", "ANSI widths for --suite, separated by spaces or commas")
	themeRaw := flag.String("theme", "default", "theme for single render/compare")
	themesRaw := flag.String("themes", "", "themes for --suite, separated by spaces or commas, or all")
	boring := flag.Bool("boring", false, "use boring output for single render/compare")
	boringsRaw := flag.String("borings", "", "boring values for --suite, separated by spaces or commas")
	osc8 := flag.Bool("osc8", false, "enable OSC8 for single render/compare")
	osc8sRaw := flag.String("osc8s", "", "OSC8 values for --suite, separated by spaces or commas")
	tableBufferRaw := flag.String("table-buffer", "full", "table buffer mode: full|row")
	tableBuffersRaw := flag.String("table-buffers", "", "table buffer modes for --suite, separated by spaces or commas")
	tableWireRaw := flag.String("table-wire", "line", "table wire mode: line|ascii|space")
	tableWiresRaw := flag.String("table-wires", "", "table wire modes for --suite, separated by spaces or commas")
	jobsRaw := flag.String("jobs", "", "parallel suite jobs for --compare-libmdf --suite; defaults to LIBMDF_PARITY_JOBS or GOMAXPROCS")
	flag.Parse()
	data, err := io.ReadAll(os.Stdin)
	if err != nil {
		fmt.Fprintf(os.Stderr, "read stdin: %v\n", err)
		os.Exit(1)
	}
	if env := os.Getenv("PARITY_CHUNK"); env != "" {
		if n, convErr := strconv.Atoi(env); convErr == nil {
			*chunk = n
		}
	}
	if *compareLibmdf {
		if *traceWrites != "" {
			fmt.Fprintf(os.Stderr, "--compare-libmdf cannot be combined with --trace-writes\n")
			os.Exit(2)
		}
		if *suite != "" {
			chunks, err := parseIntList(*chunksRaw, "LIBMDF_PARITY_CHUNKS", "1 2 3 7 31 4096")
			if err != nil {
				fmt.Fprintf(os.Stderr, "chunks: %v\n", err)
				os.Exit(2)
			}
			widths, err := parseIntList(*widthsRaw, "LIBMDF_PARITY_WIDTHS", "20 40 80 120")
			if err != nil {
				fmt.Fprintf(os.Stderr, "widths: %v\n", err)
				os.Exit(2)
			}
			themes, err := parseThemeList(*themesRaw)
			if err != nil {
				fmt.Fprintf(os.Stderr, "themes: %v\n", err)
				os.Exit(2)
			}
			borings, err := parseBoolList(*boringsRaw, "LIBMDF_PARITY_BORINGS", "false")
			if err != nil {
				fmt.Fprintf(os.Stderr, "borings: %v\n", err)
				os.Exit(2)
			}
			osc8s, err := parseBoolList(*osc8sRaw, "LIBMDF_PARITY_OSC8S", "false")
			if err != nil {
				fmt.Fprintf(os.Stderr, "osc8s: %v\n", err)
				os.Exit(2)
			}
			tableBuffers, err := parseTableBufferList(*tableBuffersRaw)
			if err != nil {
				fmt.Fprintf(os.Stderr, "table-buffers: %v\n", err)
				os.Exit(2)
			}
			tableWires, err := parseTableWireList(*tableWiresRaw)
			if err != nil {
				fmt.Fprintf(os.Stderr, "table-wires: %v\n", err)
				os.Exit(2)
			}
			jobs, err := parseJobs(*jobsRaw)
			if err != nil {
				fmt.Fprintf(os.Stderr, "jobs: %v\n", err)
				os.Exit(2)
			}
			if err := compareLibmdfSuite(*mode, *suite, chunks, widths, parityOptionSets(themes, borings, osc8s, tableBuffers, tableWires), *traceCompare, jobs); err != nil {
				fmt.Fprintf(os.Stderr, "compare: %v\n", err)
				os.Exit(1)
			}
			return
		}
		tableBuffers, err := parseTableBufferList(*tableBufferRaw)
		if err != nil {
			fmt.Fprintf(os.Stderr, "table-buffer: %v\n", err)
			os.Exit(2)
		}
		tableWires, err := parseTableWireList(*tableWireRaw)
		if err != nil {
			fmt.Fprintf(os.Stderr, "table-wire: %v\n", err)
			os.Exit(2)
		}
		opts := defaultParityOptions()
		opts.theme = *themeRaw
		opts.boring = *boring
		opts.osc8 = *osc8
		opts.tableBuffer = tableBuffers[0]
		opts.tableWire = tableWires[0]
		if err := compareLibmdfOne(*mode, data, *chunk, *width, opts, *traceCompare); err != nil {
			fmt.Fprintf(os.Stderr, "compare: %v\n", err)
			os.Exit(1)
		}
		return
	}
	tableBuffers, err := parseTableBufferList(*tableBufferRaw)
	if err != nil {
		fmt.Fprintf(os.Stderr, "table-buffer: %v\n", err)
		os.Exit(2)
	}
	tableWires, err := parseTableWireList(*tableWireRaw)
	if err != nil {
		fmt.Fprintf(os.Stderr, "table-wire: %v\n", err)
		os.Exit(2)
	}
	opts := defaultParityOptions()
	opts.theme = *themeRaw
	opts.boring = *boring
	opts.osc8 = *osc8
	opts.tableBuffer = tableBuffers[0]
	opts.tableWire = tableWires[0]
	out, err := render(*mode, data, *chunk, *width, opts, *traceWrites)
	if err != nil {
		fmt.Fprintf(os.Stderr, "render: %v\n", err)
		os.Exit(1)
	}
	_, _ = os.Stdout.Write(out)
}

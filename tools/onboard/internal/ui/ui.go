// Package ui is how the tool talks to the user: a guided conversation in a
// terminal, or plain lines with no questions for scripts.
package ui

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"

	"github.com/charmbracelet/huh"
	"github.com/charmbracelet/huh/spinner"
	"github.com/charmbracelet/lipgloss"
)

// ErrCancelled means the user stopped the tool (Ctrl-C, Esc).
var ErrCancelled = errors.New("cancelled")

// NeedsAnswerError means script mode reached a question.
type NeedsAnswerError struct{ Question, Hint string }

func (e *NeedsAnswerError) Error() string {
	if e.Hint != "" {
		return fmt.Sprintf("%s (%s)", e.Question, e.Hint)
	}
	return e.Question
}

// Option is one choice in a list.
type Option struct {
	Label string
	Value string
}

// UI is what the onboarding flow needs from the user's side.
type UI interface {
	// Title shows the tool's heading.
	Title(text string)
	// Step runs fn while showing title, then shows its result: done (✓)
	// with fn's one-line summary, or failed (✗).
	Step(title string, fn func(ctx context.Context) (string, error)) error
	// Say shows a line of information; Warn a line that needs attention.
	Say(text string)
	Warn(text string)
	// Problem explains a failure and what to do about it.
	Problem(what, advice string)
	// Choose offers options, the first selected. hint names the flag that
	// answers it in script mode.
	Choose(title, description string, options []Option, hint string) (string, error)
	// Confirm asks a yes or no question.
	Confirm(title, description string, yes bool, hint string) (bool, error)
	// Input asks for text, proposing value.
	Input(title, description, value, hint string) (string, error)
	// Secret asks for hidden text, such as a password.
	Secret(title, description, hint string) (string, error)
	// Interactive reports whether questions can be asked.
	Interactive() bool
}

var (
	titleStyle = lipgloss.NewStyle().Bold(true)
	okStyle    = lipgloss.NewStyle().Foreground(lipgloss.Color("2"))
	failStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("1"))
	warnStyle  = lipgloss.NewStyle().Foreground(lipgloss.Color("3"))
	dimStyle   = lipgloss.NewStyle().Faint(true)
)

// Guided asks questions in the terminal. Accessible mode (for screen
// readers) uses plain prompts without redrawing.
type Guided struct {
	Out        io.Writer
	Accessible bool
}

// NewGuided returns the guided UI. ACCESSIBLE=1 in the environment turns on
// accessible mode.
func NewGuided() *Guided {
	return &Guided{Out: os.Stdout, Accessible: os.Getenv("ACCESSIBLE") != ""}
}

func (g *Guided) Interactive() bool { return true }

func (g *Guided) Title(text string) {
	fmt.Fprintf(g.Out, "\n%s\n\n", titleStyle.Render(text))
}

func (g *Guided) Step(title string, fn func(ctx context.Context) (string, error)) error {
	var summary string
	err := spinner.New().Title(" " + title).Accessible(g.Accessible).
		ActionWithErr(func(ctx context.Context) error {
			var err error
			summary, err = fn(ctx)
			return err
		}).Run()
	if err != nil {
		fmt.Fprintf(g.Out, "%s %s\n", failStyle.Render("✗"), title)
		return err
	}
	line := title
	if summary != "" {
		line = summary
	}
	fmt.Fprintf(g.Out, "%s %s\n", okStyle.Render("✓"), line)
	return nil
}

func (g *Guided) Say(text string)  { fmt.Fprintf(g.Out, "  %s\n", text) }
func (g *Guided) Warn(text string) { fmt.Fprintf(g.Out, "%s %s\n", warnStyle.Render("!"), text) }

func (g *Guided) Problem(what, advice string) {
	fmt.Fprintf(g.Out, "\n%s %s\n", failStyle.Render("✗"), what)
	if advice != "" {
		fmt.Fprintf(g.Out, "  %s\n", dimStyle.Render(advice))
	}
	fmt.Fprintln(g.Out)
}

func (g *Guided) run(f huh.Field) error {
	err := huh.NewForm(huh.NewGroup(f)).WithAccessible(g.Accessible).
		WithShowHelp(true).Run()
	if errors.Is(err, huh.ErrUserAborted) {
		return ErrCancelled
	}
	return err
}

func (g *Guided) Choose(title, description string, options []Option, hint string) (string, error) {
	var value string
	opts := make([]huh.Option[string], len(options))
	for i, o := range options {
		opts[i] = huh.NewOption(o.Label, o.Value)
	}
	if len(options) > 0 {
		value = options[0].Value
	}
	err := g.run(huh.NewSelect[string]().Title(title).Description(description).
		Options(opts...).Value(&value))
	return value, err
}

func (g *Guided) Confirm(title, description string, yes bool, hint string) (bool, error) {
	value := yes
	err := g.run(huh.NewConfirm().Title(title).Description(description).
		Affirmative("Yes").Negative("No").Value(&value))
	return value, err
}

func (g *Guided) Input(title, description, value, hint string) (string, error) {
	err := g.run(huh.NewInput().Title(title).Description(description).Value(&value).
		Validate(func(s string) error {
			if s == "" {
				return errors.New("please enter a value")
			}
			return nil
		}))
	return value, err
}

func (g *Guided) Secret(title, description, hint string) (string, error) {
	var value string
	err := g.run(huh.NewInput().Title(title).Description(description).
		EchoMode(huh.EchoModePassword).Value(&value))
	return value, err
}

// Script prints one line per step and answers no questions: each question
// is an error naming the flag that answers it.
type Script struct {
	Out io.Writer
}

func (s *Script) Interactive() bool { return false }
func (s *Script) Title(text string) {}

func (s *Script) Step(title string, fn func(ctx context.Context) (string, error)) error {
	summary, err := fn(context.Background())
	if err != nil {
		fmt.Fprintf(s.Out, "FAIL %s: %v\n", title, err)
		return err
	}
	if summary == "" {
		summary = title
	}
	fmt.Fprintf(s.Out, "ok   %s\n", summary)
	return nil
}

func (s *Script) Say(text string)  { fmt.Fprintf(s.Out, "     %s\n", text) }
func (s *Script) Warn(text string) { fmt.Fprintf(s.Out, "warn %s\n", text) }
func (s *Script) Problem(what, advice string) {
	fmt.Fprintf(s.Out, "FAIL %s\n", what)
	if advice != "" {
		fmt.Fprintf(s.Out, "     %s\n", advice)
	}
}

func (s *Script) Choose(title, _ string, _ []Option, hint string) (string, error) {
	return "", &NeedsAnswerError{Question: title, Hint: hint}
}

func (s *Script) Confirm(title, _ string, _ bool, hint string) (bool, error) {
	return false, &NeedsAnswerError{Question: title, Hint: hint}
}

func (s *Script) Input(_, _, value, _ string) (string, error) { return value, nil }

func (s *Script) Secret(title, _, hint string) (string, error) {
	return "", &NeedsAnswerError{Question: title, Hint: hint}
}

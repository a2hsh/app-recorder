# Accessibility

apprecorder is written by a blind developer, and accessibility here is treated as a correctness property rather than a feature. The accessibility tree is asserted by an automated test on every run, the same way the encoders are. A shortcut cannot be documented as one key and implemented as another, because the table that dispatches the key is the same table that draws the help screen.

This page is the keyboard map, an explanation of how the canvas is navigated, and an account of why announcements work the way they do.

## Every operation is reachable by keyboard

There are no mouse-only paths. This is a hard rule, and the automated UIA test has already caught a real violation of it: the panel splitter shipped as an unnamed focusable pane that could only be dragged. It is now named, in the tab order, and driven by the arrow keys.

## The keyboard map

Press **F1** at any time for the live version of this list. It is generated from the binding tables themselves, so it cannot drift.

### The window

| Key | Action |
|---|---|
| Ctrl+N | Start a new session |
| Ctrl+O | Open a session |
| Ctrl+S | Save this session |
| Ctrl+Shift+S | Save this session under a new name |
| Ctrl+1 | Add a source |
| Ctrl+2 | Add a bus |
| Ctrl+3 | Add an output |
| Ctrl+Shift+3 | Remove an output |
| F2 | Rename this bus |
| Ctrl+R | Start recording |
| Ctrl+P | Pause the recording, leaving the file open |
| Ctrl+Shift+P | Resume the recording |
| Ctrl+Full Stop | Stop recording and close the files |
| Ctrl+T | Show or hide the structure panel |
| Ctrl+D | Switch between light and dark appearance |
| Ctrl+Shift+H | Hide the window to the notification area |
| F6 | Move to the next pane |
| F1 | Show the list of keyboard shortcuts |
| Alt+F4 | Exit |

F6 and Alt+F4 are the platform's own. Rebinding either would be worse than leaving them to Windows.

### The signal-flow canvas

| Key | Action |
|---|---|
| Tab | Move to the next node |
| Shift+Tab | Move to the previous node |
| Down Arrow | Move to the next node in this column |
| Up Arrow | Move to the previous node in this column |
| Right Arrow | Go to what this node feeds |
| Left Arrow | Go to what feeds this node |
| Ctrl+Down Arrow | Go to what this node feeds |
| Ctrl+Up Arrow | Go to what feeds this node |
| Home | Move to the first node |
| End | Move to the last node |
| Ctrl+E | Connect this node to another |
| Ctrl+Shift+E | Disconnect this node from another |
| Delete | Remove this node |
| Plus or Numpad Plus | Make this source louder |
| Minus or Numpad Minus | Make this source quieter |
| Escape | Cancel what is half done |
| Enter or Space | Say this node and its connections again |

Two of those rows are the same move written twice, and that is deliberate. **Left and Right are geometric**: they follow the picture, and in a right-to-left layout they swap, because the arrow that points downstream in Arabic is Left. **Ctrl+Up and Ctrl+Down are logical**: upstream and downstream, never mirrored, in any language. Both routes exist so that nobody navigating by keyboard has to know which way the drawing happens to face.

### The panel divider

Left and Right arrows move it, Ctrl with them takes a larger step, Home and End give it its narrowest and widest. It has a name and it is in the tab order, because a divider you cannot reach is a panel you cannot resize.

## The panes

F6 cycles between them. Each has a name and a spoken description.

- **Signal flow** — the recording graph, drawn as nodes.
- **Structure** — the same graph as a tree: every bus, the sources feeding it, and the files it saves.
- **Recording status** — whether a recording is running, how long it has been going, and how each source is doing.
- **Panel divider**
- **Status** — the status bar, which is also the announcement channel; see below.

The canvas and the tree are **two real views over one model**, not a visual view with an accessible fallback. Neither is derived from the other; both read the same graph, and a change made in one appears in the other. That was the point of the design: a separate accessible tree beside an inaccessible canvas would be separate-but-equal, and the canvas would never get fixed.

## How the canvas is navigable

**Every node is a real Win32 child window.** Not a rectangle painted on a canvas — an actual `HWND`. That is why focus, tab order and the accessibility tree exist at all: the platform supplies them. Nothing here is a hand-written accessibility provider that could quietly stop matching what is drawn.

Three things that "real windows" do not give you for free, each of which is silent when it is wrong, and each of which had to be handled explicitly:

- **A custom window class is reported as a nameless pane.** The role has to be annotated, or a screen reader lands on a node and says "pane". apprecorder annotates it, and the test asserts the control type of every element.
- **The accessibility tree is enumerated in Z-order, and each new child window stacks on top of the last.** Creating nodes in logical order therefore produces a *reversed* tree — outputs first, sources last. The canvas restacks explicitly after building the graph.
- **Windows' dialog manager eats Escape, Enter and Tab** before any node sees them. Escape in particular would reach the frame as a cancel and strand you inside a half-made connection. Canvas nodes therefore claim every key, and the canvas implements Tab itself — including handing focus *out* of the pane at either end, which is the part that keeps it from being a focus trap.

### Edges

An edge between two nodes is painted, and a painted line has no element to land on. So **every edge is in the text of the node at each end of it**. A source says what it feeds; a bus says what feeds it and what it saves; an output says which bus it is written from. If the connection is not in that sentence, it does not exist for a screen reader — so it is always in that sentence.

The node's text also carries its **kind**, because kind is otherwise carried by colour, and colour is never allowed to be the only carrier of a fact. The same rule governs the recording state: a pause is announced in words, never left to an icon or a change of colour, because a pause you cannot hear is an hour of a meeting that was never recorded and it looks exactly like a recording that is going fine.

### Making a connection

Connecting is a two-step gesture rather than a drag. On a source, press Ctrl+E. apprecorder says "Connecting from Teams. Move to a bus and press Control and E again, or press Escape to cancel." Move to the bus, press Ctrl+E, and it says "Teams now feeds Main Mix." Ctrl+Shift+E does the same thing in reverse.

Every refusal is a sentence that says what actually happened — "a connection starts at a source, and Main Mix is not one" — rather than a generic decline.

## Why announcements work the way they do

**apprecorder never speaks directly.** There is no SAPI, no NVDA controller client, no Tolk, no text-to-speech of any kind in this product. UIA and the shell hand *semantics* to the screen reader, which then applies the user's own voice, rate, verbosity, interruption rules and braille routing. Speaking directly bypasses every one of those and only works for the readers you happen to have a driver for. It also means that with no screen reader running there is simply no consumer, and therefore nothing to detect and nothing to go wrong.

That leaves three channels, and apprecorder uses all three, because none of them is reliable on its own.

**The focused element's name.** When something changes, the focused node's own name changes with it. A name change on the focused element is the one mechanism every screen reader honours.

**The status bar as a live region.** This is the single owner of "say this to the user". It took a measurement to get right, and the mistake is worth recording because it produced silence rather than an error: what a screen reader *does* with a live-region event is read the element's **name**. While the status bar's name was the fixed word "Status", every announcement the product made was inert — the event fired, the reader said "Status", and the sentence was never heard by anybody. The sentence is now the element's accessible name, and both a name-change event and a live-region event are raised, because different readers honour different ones.

**Notification-area balloons, for anything that happens in the background.** A live-region change on an unfocused background window is not reliably announced by any screen reader — and a recording spends its entire life in exactly that state. So a source that dies, a source that comes back, a source that turns out to be muted, an output that had to be renamed, a recording that started or stopped: all of it also goes out as a balloon. Screen readers announce those reliably, sighted users see them too, and it uses the same shell API as the icon.

The tray icon's tooltip is a fourth, quieter channel: it updates every second with the state and the elapsed time, and Windows+B then the arrow keys reads it. That is how you check on a three-hour recording from inside another application without raising a window and without anything interrupting you.

## Visual design and the accessibility tree

Custom painting is allowed to change how a control looks and is not allowed to change what it *is*. Standard controls are custom-drawn, never owner-drawn: custom draw alters colours and fonts and preserves the control's semantics; full owner-draw replaces them and leaves a screen reader nothing to read.

High contrast and the selected state fall back to the system's own painting rather than to apprecorder's palette.

Focus indicators are two-coloured. That is a correctness property rather than a decoration: no single colour clears a 3:1 contrast ratio against the surface *and* the accent *and* the selection at the same time.

Dark mode follows the documented Windows preference, and every part of it is non-fatal — a failure in the theming code can never take down the application.

## It is tested, not asserted

`tests/test_ui_a11y.c` is a real UI Automation client. It raises the window on one thread and walks the tree from another — two threads is required, because a UIA client querying a window on the thread that owns it can deadlock.

It asserts that **every element has a non-empty name and a correct control type**, counts how many are keyboard-focusable, and prints the whole tree on every run. A screen reader sees exactly what UIA exposes, so this tests the property directly rather than by proxy. It earned its keep on its first run by finding the unreachable splitter.

Two limits are honest and documented in the test itself. The operating system's own title-bar element has an empty name in every Win32 application, so that subtree is marked as the OS's and exempted. And when the test process is not in the foreground, UIA reports no focused element, so the focus check falls back to a per-thread query and says which route it used.

## Language and layout

Everything above is written so that Arabic can be added without rebuilding it. No user-facing string is a literal in the code; every one lives in a catalog with positional inserts a translator can reorder, and the plural API carries all six Arabic forms rather than English's two.

Layout direction is a parameter, not a constant. In a right-to-left layout the graph reads right to left — sources on the right, outputs on the left — and the geometric arrow keys swap with it. **Logical order never flips.** Tab order, the accessibility tree and the structure panel stay source → bus → action in every language. Only the painting mirrors.

**Arabic is not shipped yet.** Roughly 500 catalog strings are still awaiting translation, and the build reports the outstanding count. Today apprecorder runs in English.

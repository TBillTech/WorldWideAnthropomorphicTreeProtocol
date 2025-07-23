# Project Planning

## Currently Active Projects

- **MORPHEUS**: Needs more thought
- **POET**: Books, and _could_ use WWATP  
- **LANGUAGE BARRIER**: World Wide Anthropomorphic Tree Protocol
- **ZODIAK**: Also uses WWATP
- **LEVELZERO**: Also uses WWATP

## Full Task Set for Operation POET Capability Set

### Completed Tasks
- ✅ Thread safe Backend at the DAG root, using Simple Backend with read and write
- ✅ Composite & Redirection Backends, for restricting the Static HTML Domain
- ✅ HTTP3_Client Backend in cpp
- ✅ HTTP3_Server Frontend, for supporting HTTP3_Client cpp, HTTP3_Client javascript, Flat HTML Mediator, Static HTML Watcher, Flat SSML Mediator, DOMLink Frontend
- ✅ File Backend, for supporting writing out YAML and HTML files, which is isomorphic to directories with a node.txt and contents
- ✅ Cloning Mediator Frontend + (if lazy flag, use earlier version notification in one tree to cause update)
- ✅ YAML Mediator Frontend (WWATP Trees implicitly use MathJSON "representation", declarative Chart "representation")

### Remaining Tasks
- **Server Executable** _(1 week)_
- **HTTP3_Client Backend in javascript** _(3 weeks)_ - The ONLY javascript backend
- **Flat HTML Mediator Frontend** _(1 week)_ - Using MathLive for LaTeX front ↔ MathJSON back, PlotLY for declarative PlotLy front ← declarative Chart back
- **DOMLink Frontend** _(2 weeks)_ - Also uses MathLive LaTeX DOM, implying HTTP3ServerFrontend should convert LaTeX front ↔ MathJSON back, PlotLY for declarative PlotLy front ← declarative Chart back
- **Careful Prose Tool Mediator Frontend** _(2 weeks)_ - In a different project
- **Static HTML Watcher Frontend** _(0.5 weeks)_ - Using MathLive for HTML front ← LaTeX ← MathJSON back to LaTeX to HTML, PlotLY for PNG ← declarative PlotLy front ← declarative Chart back, and can use pandoc to convert to EPUB
- **Flat SSML Mediator Frontend** _(0.5 weeks)_ - Simply converts DOM elements into SSML markdown

**Remaining Time:** 11 weeks ≈ 4 months ≈ Nov 10

## High Level Project Planning (and Retrospective)

### Current Goals
- **Week of Aug 3-9th**: Aestivation vacation! 🏖️
- **Operation POET via WWATP**: ~4 month project remaining. Even if this isn't a good way to write a book, I can still write papers this way, and it will almost all be leveraged by web applications and Unreal Engine client
- **Monday Goal**: Finish debugging YAML Mediator Frontend and unit tests
- **Wednesday Goal**: Compile Server Executable with no features, and write up requirements

## Retrospective Lessons

### 7/17/2025
Third party YAML library created excess technical debt due to quirky data structure behavior. Time estimate was roughly 3x too short.

### 7/10/2025  
Unit tests saved my bacon for refactoring! Also, dealing with actual ground truth on inotify was a real pain, given the quirky behavior, and caused an extra 6 hours of effort to build complex maps from actual inotify results back to desired notifications and the reverse, and keeping track of all the handles.

### 6/26/2025
The QUIC library integration is inducing some technical debt, while the majority of bugs are mine. Bugs having to do with the transport layer are difficult and complex, increasing time to complete substantially when working on the transport layer.

### 6/19/2025
Had some problems debugging seg faults, and turned on all warnings and debug settings. It worked very well to find bad code bugs.

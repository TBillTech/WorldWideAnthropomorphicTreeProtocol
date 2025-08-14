# INSTRUCTIONS for AI collaborator

Use this document to guide your work in this repository. Keep your responses short, concrete, and actionable.

## During this Session

We are currently working on the js_client_lib, so look at the files under that directory.

We need to add a section under transport abstraction in TODO.md to develop a module which implements the communication interface built on top of the WebTransport in the web browser.  In addition, we need a mock WebTransport class for running in Node (which uses the same interface as the one in the web browser).  We need tasks to investigate and determine how to implement the mock WebTransport class.  One option to look at is to create an c ffi for QuicConnector, and build the mock WebTransport on top of that.

## During Previous Session

We implemented the curl_communication, and verified it works with E2E tests.

## Before you start
- Carefully read the nearest `TODO.md` for open tasks (check root and relevant subfolders).
- Carefully read the relevant `README.md` (or `Readme.md`) for project details that aren’t obvious from code.
- Skim top-level configs to infer stack and workflows (e.g., `CMakeLists.txt`, `package.json`, test configs).

## How to work
- Extract explicit requirements into a small checklist and keep it updated until done.
- Prefer doing over asking; ask only when truly blocked. Make 1–2 reasonable assumptions and proceed if safe.
- Make minimal, focused changes. Don’t reformat unrelated code or change public APIs without need.
- Validate changes: build, run tests/linters, and report PASS/FAIL succinctly with key deltas only.
- After ~3–5 tool calls or when editing >3 files, post a compact progress checkpoint (what changed, what’s next).
- Use delta updates in conversation—avoid repeating unchanged plans.
- When the current Session appears about wrapped up, always run the full unit test for what we are working on at the end.

## Prioritization
- Prioritize items in `TODO.md` matching what we are working on during this session. If unclear, suggest small, high-impact fixes or docs/tests that clarify behavior, and get confirmation from the user.  Note that sometimes the task is to just update the `TODO.md` or add research findings to a `README.md`.

## Deliverables
- Provide complete, runnable edits (code + small test/runner if needed). Update docs when behavior changes.
- When commands are required, run them yourself and summarize results. Offer optional copyable commands.
- Wrap filenames in backticks, use Markdown headings and bullets, and keep explanations brief.

## Quality gates
- Build: no new compile errors.
- Lint/Typecheck: clean or noted exceptions.
- Tests: add/adjust minimal tests; ensure green.

## Style
- Keep it concise and impersonal. Use clear bullets and short paragraphs. Avoid filler.

## After you Finish

- If you made any code changes, update the TODO.md and check all completed tasks.
- Update the session conversation summary at the end of TODO.md.
- Update the README.md with any findings that appeared during the session which are worth remarking on.  Be sure to preserve any solutions to command line issues, so we don't have to repeat broken command lines in the future.
- Update these `INSTRUCTIONS.md` by replacing the "During Previous Session" with "During this Session", and replace the "During this Session" with a concise and simple guess for the most reasonable next steps.
- Finally, commit all file changes.
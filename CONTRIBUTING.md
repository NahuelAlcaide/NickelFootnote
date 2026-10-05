# Contributing

Thanks for helping! Bug reports, compatibility reports and pull requests are
all welcome.

## Reporting a bug

Use the [bug report form](https://github.com/NahuelAlcaide/NickelFootnote/issues/new?template=bug_report.yml)
and attach `.adds/nickelfootnote/log.txt` from the Kobo. It never contains your
tokens or questions, but skim it before posting. If Nickel crashed, also
attach `.kobo/stack_00.log`.

## Trying it on another device or firmware

Please [file a compatibility report](https://github.com/NahuelAlcaide/NickelFootnote/issues/new?template=compatibility_report.yml),
whether it works or not. The log's first lines show which hooks were found.

## Pull requests

1. Build with `.\build.ps1` (or the `docker run` line in the README). The
   build uses `-Werror`, so it must compile without warnings.
2. Test on a real device and say which device and firmware in the PR. There is
   no emulator, so a PR that wasn't run on a Kobo needs extra care in review.
3. Read [docs/DEVELOPING.md](docs/DEVELOPING.md), especially "Lessons
   learned". Wrap risky new code in the crash guards, and keep Nickel calls
   out of the startup grace period.
4. If you change the model instructions in `src/ask.cc`, make the same
   change in `tools/signin.py`.
5. Match the surrounding style: 4-space indents, short comments that say why.

By contributing, you agree that your contributions are licensed under the
GPL-3.0-or-later, like the rest of the project.

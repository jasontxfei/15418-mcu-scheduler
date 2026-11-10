# 15-418 Project Website

Workload- and Memory-Aware Scheduler for Heterogeneous Embedded MCU Cores

Jason Tang (jasont2) and Catherine Li (catheri5) · Fall 2026

- Website: https://jasontxfei.github.io/15418-mcu-scheduler/
- Proposal (HTML): https://jasontxfei.github.io/15418-mcu-scheduler/proposal.html
- Proposal (PDF): https://jasontxfei.github.io/15418-mcu-scheduler/proposal.pdf

The site uses plain HTML and default browser styling. No CSS, JavaScript, dependencies, or build step are required to serve it.

## Edit

- `docs/index.html`: homepage, report links, and schedule.
- `docs/proposal.html`: HTML version of the proposal.
- `docs/proposal.pdf`: committed proposal PDF.
- `docs/proposal.tex`: LaTeX source for the PDF.

The HTML proposal and homepage schedule reflect the committed PDF and its LaTeX source. Keep all versions in sync when revising the proposal. Add later team-written reports to `docs/` and link them from the homepage.

Local preview: `python3 -m http.server 8000 --directory docs`.

## Hosting and submission

GitHub repository Settings → Pages → Deploy from a branch → `main` → `/docs` → Save.

Submit the full proposal PDF to Gradescope as a group. Publishing the website does not submit the proposal. See the [course handout](https://www.cs.cmu.edu/~418/projects/project-proposal.pdf) for requirements.

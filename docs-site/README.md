# Cortrix documentation site

Source for the Cortrix documentation site at <https://cortrix.ai/docs/>, built
with the [OINK](https://oink.pgsty.com/) Hugo theme.

OINK is a separate project under the Apache License 2.0, derived from
[Docsy](https://github.com/google/docsy). Its license and notices are in the
[theme repository](https://github.com/pgsty/oink), and every page credits the
theme in the footer.

## Requirements

- Hugo Extended 0.168.0 or newer (the `Docs` workflow uses 0.168.0)
- Go 1.27.3 or newer
- Git
- Python 3 with PyYAML

## Preview

The API reference renders a filtered copy of the OpenAPI spec. Generate it
first, and again whenever `api/` changes. It needs Python 3 with PyYAML.

```bash
python3 docs-site/scripts/filter_openapi.py api docs-site/.openapi
cd docs-site
hugo server
```

Open `http://localhost:1313/docs/`.

## Strict build

```bash
docs-site/scripts/build_strict.sh
```

The script runs the production build and fails on any Hugo warning or error
except one: Hugo 0.168 reports that LibSass is deprecated, because the OINK
theme compiles its styles with the LibSass built into Hugo Extended.

## Layout

```text
docs-site/
├── hugo.yaml          site configuration
├── go.mod             pins the OINK theme module
├── scripts/           filter_openapi.py writes the spec copy the site renders;
│                      build_strict.sh runs the strict build
├── data/home/         landing page content
├── assets/            logo and theme styles
└── content/docs/      documentation pages
```

Pages in `content/docs/` are published without the `docs` section in their URL:
`content/docs/get-started/quickstart.md` is served at
`/docs/get-started/quickstart/`. Links between pages are written as
`/docs/<section>/<page>/`.

The `Docs` workflow runs the strict build on every pull request that changes
`docs-site/` or `api/`, and publishes the site when a change reaches
`release/1.0`.

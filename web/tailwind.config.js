/** @type {import('tailwindcss').Config} */
export default {
  content: ['./index.html', './src/**/*.{js,ts,jsx,tsx}'],
  darkMode: 'class',
  theme: {
    extend: {
      colors: {
        // ─── Cortrix Magma VI — semantic tokens (SoT: design/vi-mockup/
        //     cortrix-vi-console.html). Driven by CSS variables in index.css:
        //     `:root` = light/day, `html.dark` = dark/night. Use the
        //     `rgb(var(--x) / <alpha-value>)` form so Tailwind opacity
        //     modifiers (`bg-magma/10`, `border-line/40`) work in both themes.
        bg: 'rgb(var(--bg) / <alpha-value>)',
        bgalt: 'rgb(var(--bgalt) / <alpha-value>)',
        surface: 'rgb(var(--surface) / <alpha-value>)',
        surface2: 'rgb(var(--surface2) / <alpha-value>)',
        line: 'rgb(var(--line) / <alpha-value>)',
        txt: 'rgb(var(--txt) / <alpha-value>)',
        muted: 'rgb(var(--muted) / <alpha-value>)',
        magma: 'rgb(var(--magma) / <alpha-value>)',
        'magma-h': 'rgb(var(--magma-h) / <alpha-value>)',
        amber: 'rgb(var(--amber) / <alpha-value>)',
        codebg: 'rgb(var(--codebg) / <alpha-value>)',
        ok: 'rgb(var(--ok) / <alpha-value>)',

        // ─── Cortrix brand palette — Magma Orange scale.
        //
        // Compatibility strategy: existing components reference the numeric
        // scale `cortrix-50..900` plus the named aliases `cortrix-primary*`.
        // We KEEP both shapes (0 break) but re-tint them from the old blue
        // (#5c7cfa / #2E5BFF family) to the Magma orange ramp so every legacy
        // usage (`bg-cortrix-600`, `text-cortrix-700`, `bg-cortrix-50`, …)
        // resolves to orange automatically. New UI prefers the semantic
        // tokens above (`magma`, `magma-h`).
        cortrix: {
          // Named aliases (brand) — Magma orange
          primary: '#FF6405',       // --magma
          'primary-dark': '#EA5000', // --magma-h (light) / deeper orange
          'primary-light': '#FF8A30', // --magma-h (dark) / brighter orange
          // Numeric Magma ramp (50 light → 900 deep)
          50: '#FFF4ED',
          100: '#FFE6D5',
          200: '#FFC9A8',
          300: '#FFA670',
          400: '#FF8A30',
          500: '#FF6405',
          600: '#EA5000',
          700: '#C2410C',
          800: '#9A3412',
          900: '#7C2D12',
        },

        // ─── Semantic status colors (retuned to the Magma palette).
        //     success/ok = emerald, warning ≈ amber, error = red, info → amber
        //     (no blue in the brand surface — info maps onto the amber accent).
        success: '#059669',
        warning: '#D97706',
        error: '#EF4444',
        info: '#D97706',

        // ─── Legacy dark.* surface tokens — remapped onto the Magma dark
        //     theme so existing `dark:bg-dark-surface` / `dark:text-dark-text`
        //     etc. render the mockup's night palette (no separate migration of
        //     every component needed; 0 blue residual).
        dark: {
          bg: '#0B0F19',      // --bg (dark)
          surface: '#131B2A', // --surface (dark)
          card: '#1A2236',    // --surface2 (dark)
          border: '#1E293B',  // --line (dark)
          hover: '#1A2236',   // --surface2 (dark)
          text: '#F1F5F9',    // --txt (dark)
          muted: '#94A3B8',   // --muted (dark)
        },
      },
      fontFamily: {
        // VI mockup § head — Inter (sans) + JetBrains Mono (mono), loaded via
        // Google Fonts <link> in index.html (no npm font packages).
        sans: ['Inter', '-apple-system', 'BlinkMacSystemFont', 'Segoe UI', 'sans-serif'],
        mono: ['JetBrains Mono', 'Fira Code', 'Consolas', 'monospace'],
      },
    },
  },
  plugins: [],
};

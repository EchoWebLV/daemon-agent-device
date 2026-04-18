/** @type {import('tailwindcss').Config} */
export default {
  content: ['./src/**/*.{ts,tsx}', './popup.html', './sidepanel.html'],
  theme: {
    extend: {
      colors: {
        daemon: {
          bg: '#111111',
          surface: '#1a1a1a',
          accent: '#1361f7',
          'accent-light': '#4d8aff',
          'accent-dim': '#0e4ad4',
          border: '#2a2a2a',
          dim: '#888888',
          text: '#e0e0e0',
        },
      },
      fontFamily: {
        mono: ['"JetBrains Mono"', '"SF Mono"', '"Cascadia Mono"', 'monospace'],
        sans: ['Inter', '-apple-system', 'BlinkMacSystemFont', '"Segoe UI"', 'sans-serif'],
      },
    },
  },
  plugins: [require('@tailwindcss/typography')],
};

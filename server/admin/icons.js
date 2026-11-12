// Stroke icons drawn for Multitap on a 24 px grid. The same path data renders
// in the administrator console (DOM) and in the server-rendered landing page
// (markup), so neither page needs icon fonts, data: URIs or inline styles.
export const icons = {
  grid: ['M4 4h6.5v6.5H4z', 'M13.5 4H20v6.5h-6.5z', 'M4 13.5h6.5V20H4z', 'M13.5 13.5H20V20h-6.5z'],
  monitor: ['M4 5h16a1 1 0 0 1 1 1v9a1 1 0 0 1-1 1H4a1 1 0 0 1-1-1V6a1 1 0 0 1 1-1z', 'M9 20h6', 'M12 16v4'],
  phone: ['M8 3h8a1.5 1.5 0 0 1 1.5 1.5v15A1.5 1.5 0 0 1 16 21H8a1.5 1.5 0 0 1-1.5-1.5v-15A1.5 1.5 0 0 1 8 3z', 'M11 18h2'],
  user: ['M12 12a4 4 0 1 0 0-8 4 4 0 0 0 0 8z', 'M4.5 20a7.5 7.5 0 0 1 15 0'],
  swap: ['M4 8h15', 'M15 4l4 4-4 4', 'M20 16H5', 'M9 12l-4 4 4 4'],
  cube: ['M12 3l7.8 4.5v9L12 21l-7.8-4.5v-9z', 'M12 12l7.8-4.5', 'M12 12v9', 'M12 12L4.2 7.5'],
  refresh: ['M19.5 12a7.5 7.5 0 1 1-2.2-5.3', 'M19.8 4v4.8H15'],
  list: ['M9 6h11', 'M9 12h11', 'M9 18h11', 'M4.5 6h.01', 'M4.5 12h.01', 'M4.5 18h.01'],
  sun: ['M12 16a4 4 0 1 0 0-8 4 4 0 0 0 0 8z', 'M12 2.5v2', 'M12 19.5v2', 'M2.5 12h2', 'M19.5 12h2', 'M5.3 5.3l1.4 1.4', 'M17.3 17.3l1.4 1.4', 'M5.3 18.7l1.4-1.4', 'M17.3 6.7l1.4-1.4'],
  moon: ['M20 14.5A8.5 8.5 0 0 1 9.5 4a8.5 8.5 0 1 0 10.5 10.5z'],
  download: ['M12 4v11', 'M7 10.5l5 5 5-5', 'M5 20h14'],
  logout: ['M10 4H6a2 2 0 0 0-2 2v12a2 2 0 0 0 2 2h4', 'M15 8l4 4-4 4', 'M19 12H9'],
  search: ['M11 18a7 7 0 1 0 0-14 7 7 0 0 0 0 14z', 'M20 20l-4-4'],
  filter: ['M4 6h16', 'M7 12h10', 'M10 18h4'],
  plus: ['M12 5v14', 'M5 12h14'],
  check: ['M5 12.5l4.5 4.5L19 7.5'],
  alert: ['M12 4l9 16H3z', 'M12 10v4', 'M12 17h.01'],
  info: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z', 'M12 11v5', 'M12 8h.01'],
  x: ['M6 6l12 12', 'M18 6L6 18'],
  inbox: ['M4 13l2.5-7.5h11L20 13', 'M4 13v5h16v-5', 'M4 13h4.5l1 2h5l1-2H20'],
  server: ['M4 4h16v6H4z', 'M4 14h16v6H4z', 'M8 7h.01', 'M8 17h.01'],
  shield: ['M12 3l7.5 3v5.5c0 4.5-3.2 8-7.5 9.5-4.3-1.5-7.5-5-7.5-9.5V6z'],
  power: ['M12 3v8', 'M6.6 6.6a7.5 7.5 0 1 0 10.8 0'],
  activity: ['M3 12h4l3-7 4 14 3-7h4'],
  link: ['M10 14a4 4 0 0 0 5.7 0l3-3a4 4 0 0 0-5.7-5.7l-1 1', 'M14 10a4 4 0 0 0-5.7 0l-3 3a4 4 0 0 0 5.7 5.7l1-1'],
  arrow: ['M5 12h14', 'M13 6l6 6-6 6'],
  lock: ['M6 11h12v9H6z', 'M8.5 11V8a3.5 3.5 0 0 1 7 0v3'],
  menu: ['M4 7h16', 'M4 12h16', 'M4 17h16'],
  back: ['M19 12H5', 'M11 6l-6 6 6 6'],
  chevron: ['M9 6l6 6-6 6'],
  clock: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z', 'M12 7.5V12l3 2'],
  play: ['M8 5.5v13l10.5-6.5z'],
  square: ['M7 7h10v10H7z'],
  edit: ['M4 20h4L19 9l-4-4L4 16z', 'M13.5 6.5l4 4'],
  key: ['M14.5 14a5 5 0 1 0-4.6-3L3.5 17.5V20.5h3v-2h2v-2h2l1.3-1.3a5 5 0 0 0 2.7.8z', 'M16 8h.01'],
  trash: ['M4.5 7h15', 'M9.5 7V4.5h5V7', 'M6.5 7l1 12.5h9l1-12.5'],
  users: ['M9 11a3.5 3.5 0 1 0 0-7 3.5 3.5 0 0 0 0 7z', 'M2.5 20a6.5 6.5 0 0 1 13 0', 'M16 4.3a3.5 3.5 0 0 1 0 6.4', 'M18 14a6.5 6.5 0 0 1 3.5 6'],
  gauge: ['M4 17a8 8 0 1 1 16 0', 'M12 17l4-5.5', 'M12 17h.01'],
  relay: ['M6.5 12h3', 'M14.5 12h3', 'M6.5 12a2.5 2.5 0 1 1-5 0 2.5 2.5 0 0 1 5 0', 'M14.5 12a2.5 2.5 0 1 1-5 0 2.5 2.5 0 0 1 5 0', 'M22.5 12a2.5 2.5 0 1 1-5 0 2.5 2.5 0 0 1 5 0'],
  clipboard: ['M9 4.5h6v3H9z', 'M9 6H6.5v14h11V6H15'],
  chart: ['M4 20h16', 'M7 20v-6', 'M12 20V5', 'M17 20v-9'],
  image: ['M4 5h16v14H4z', 'M4 16l4.5-4.5 3.5 3.5 3-3 5 5', 'M9 9h.01'],
  upgrade: ['M12 21a9 9 0 1 0 0-18 9 9 0 0 0 0 18z', 'M12 16.5V8', 'M8.5 11.5L12 8l3.5 3.5'],
  megaphone: ['M4 10v4h3l7 4V6l-7 4z', 'M17.5 9.5a4 4 0 0 1 0 5', 'M7.5 14v4.5h2.6V15.5'],
};

const svgNamespace = 'http://www.w3.org/2000/svg';

// Markup for server-rendered pages. Names are fixed keys of `icons`; nothing
// supplied by a request or manifest reaches this string.
export function iconMarkup(name, className = 'icon') {
  const paths = icons[name];
  if (!paths) throw new Error('unknown_icon');
  return `<svg class="${className}" viewBox="0 0 24 24" aria-hidden="true" focusable="false">${paths.map(d => `<path d="${d}"/>`).join('')}</svg>`;
}

export function createIcon(name, className = 'icon') {
  const paths = icons[name];
  if (!paths) throw new Error('unknown_icon');
  const svg = globalThis.document.createElementNS(svgNamespace, 'svg');
  svg.setAttribute('class', className); svg.setAttribute('viewBox', '0 0 24 24');
  svg.setAttribute('aria-hidden', 'true'); svg.setAttribute('focusable', 'false');
  for (const d of paths) { const path = globalThis.document.createElementNS(svgNamespace, 'path'); path.setAttribute('d', d); svg.append(path); }
  return svg;
}

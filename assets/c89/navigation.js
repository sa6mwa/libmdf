(() => {
  const banner = document.querySelector('.lights-out-banner');
  const previous = document.querySelector('#previous-heading');
  const next = document.querySelector('#next-heading');
  if (!banner || !previous || !next) return;

  const headings = [];
  for (const heading of document.querySelectorAll('.mdf-heading[data-mdf-heading]')) {
    const level = Number(heading.getAttribute('data-mdf-heading'));
    heading.setAttribute('aria-label', heading.textContent.trim().replace(/^#{1,6}\s*/, ''));
    if (level <= 2) headings.push(heading);
  }

  function scrollBehavior() {
    return window.matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth';
  }

  function move(direction) {
    const bannerBottom = banner.getBoundingClientRect().bottom;
    const target = direction > 0
      ? headings.find(heading => heading.getBoundingClientRect().top > bannerBottom + 56)
      : headings.slice().reverse().find(heading => heading.getBoundingClientRect().top < bannerBottom + 8);

    if (target) {
      target.scrollIntoView({ behavior: scrollBehavior(), block: 'start' });
    } else if (direction < 0) {
      window.scrollTo({ top: 0, behavior: scrollBehavior() });
    }
  }

  previous.addEventListener('click', () => move(-1));
  next.addEventListener('click', () => move(1));
  document.addEventListener('keydown', event => {
    if (event.key !== 'ArrowUp' && event.key !== 'ArrowDown') return;
    if (event.altKey || event.ctrlKey || event.metaKey || event.shiftKey) return;
    const target = event.target;
    if (target?.isContentEditable || /^(INPUT|TEXTAREA|SELECT)$/.test(target?.tagName || '')) return;
    event.preventDefault();
    if (!event.repeat) move(event.key === 'ArrowDown' ? 1 : -1);
  });
})();

/* HALO motion.
   Pages and setup steps wipe in behind a slanted edge that runs parallel
   to the seam: outward from the seam when moving forward, back toward it
   when moving back. Old content leaves in 110 ms. Only clip and opacity
   are animated on pages, never transform, so everything positioned
   against the ring stays put while it wipes.
   Entrances decelerate, exits are short. Reduced motion: opacity only. */
window.MOTION = (() => {
  const DEC = 'cubic-bezier(.05, .7, .1, 1)';
  const ACC = 'cubic-bezier(.3, 0, .8, .15)';
  const STD = 'cubic-bezier(.2, 0, 0, 1)';
  const POP = 'cubic-bezier(.34, 1.4, .64, 1)';
  const wipe = (inMs, outMs) => ({
    in: { ms: inMs, ease: STD, delay: 70, frames: d => d > 0
      ? [{ opacity: .3, clipPath: 'polygon(-40% 0, -3% 0, -27% 100%, -40% 100%)' }, { opacity: 1, clipPath: 'polygon(-40% 0, 152% 0, 128% 100%, -40% 100%)' }]
      : [{ opacity: .3, clipPath: 'polygon(152% 0, 160% 0, 160% 100%, 128% 100%)' }, { opacity: 1, clipPath: 'polygon(-3% 0, 160% 0, 160% 100%, -27% 100%)' }] },
    out: { ms: outMs, ease: ACC, frames: () => [{ opacity: 1 }, { opacity: 0 }] }
  });
  const axis = (dist, inMs, outMs) => ({
    in:  { ms: inMs, ease: DEC, delay: 40, frames: d => [{ opacity: 0, transform: `translateX(${d * dist}rem)` }, { opacity: 1, transform: 'none' }] },
    out: { ms: outMs, ease: ACC, frames: d => [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: `translateX(${-d * dist * .5}rem)` }] }
  });
  const fade = (inMs, outMs) => ({
    in:  { ms: inMs, ease: STD, frames: () => [{ opacity: 0 }, { opacity: 1 }] },
    out: { ms: outMs, ease: STD, frames: () => [{ opacity: 1 }, { opacity: 0 }] }
  });
  const bloom = { // content appearing inside the ring
    in:  { ms: 420, ease: DEC, delay: 80, frames: () => [{ opacity: 0, transform: 'scale(.9)' }, { opacity: 1, transform: 'none' }] },
    out: { ms: 160, ease: ACC, frames: () => [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'scale(.96)' }] }
  };
  return {
    defaultTheme: 'dark',
    page:  wipe(520, 110),
    step:  wipe(560, 110),
    tab:   axis(2, 300, 120),
    mode:  fade(460, 200),
    sheet: bloom,
    launch: bloom,
    scrim: fade(300, 200),
    toast: { in: { ms: 420, ease: POP, frames: () => [{ opacity: 0, transform: 'translateY(-1rem) scale(.92)' }, { opacity: 1, transform: 'none' }] },
             out: { ms: 200, ease: ACC, frames: () => [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateY(-.5rem) scale(.96)' }] } },
    alert: { in: { ms: 360, ease: DEC, frames: () => [{ opacity: 0, transform: 'translateY(.8rem)' }, { opacity: 1, transform: 'none' }] },
             out: { ms: 150, ease: ACC, frames: () => [{ opacity: 1 }, { opacity: 0 }] } },
    fade:  fade(220, 120),
    cycle: { in: { ms: 260, ease: DEC, frames: d => [{ opacity: 0, transform: `translateX(${d * .9}rem)` }, { opacity: 1, transform: 'none' }] } },
    expand:{ in: { ms: 320, ease: DEC, frames: h => [{ height: '0px', opacity: 0 }, { height: h + 'px', opacity: 1 }] },
             out: { ms: 200, ease: ACC, frames: h => [{ height: h + 'px', opacity: 1 }, { height: '0px', opacity: 0 }] } },
    drawer:{ in: { ms: 380, ease: DEC, frames: () => [{ opacity: 0, transform: 'translateX(2rem)' }, { opacity: 1, transform: 'none' }] },
             out: { ms: 160, ease: ACC, frames: () => [{ opacity: 1, transform: 'none' }, { opacity: 0, transform: 'translateX(1rem)' }] } },
    reduced: {
      fade:  fade(140, 90),
      cycle: { in: { ms: 120, frames: () => [{ opacity: .2 }, { opacity: 1 }] } },
      expand:{ in: { ms: 1, frames: () => [{ opacity: 1 }, { opacity: 1 }] }, out: { ms: 1, frames: () => [{ opacity: 1 }, { opacity: 1 }] } }
    }
  };
})();

// Bench bot for Bit-Rate Rush 3KB, injected into the game's scope by --inject.
// Presses A on menus, otherwise runs from enemies and towards bits. God mode
// keeps the run going through the boss fight.
{
  let frame = 0;
  globalThis.__bot = () => {
    const f = frame++;
    if (st === 1) hp = 99;
    if (f < 30 || st !== 1) return f % 20 === 0 ? 16 : 0;
    let fx = Math.cos(f / 200) * 0.4, fy = Math.sin(f / 200) * 0.4, bd = 1e9;
    for (const d of D) {
      const q = (d.x - x) ** 2 + (d.y - y) ** 2;
      if (q < bd) {
        bd = q;
        fx = (d.x - x) / Math.sqrt(q + 1);
        fy = (d.y - y) / Math.sqrt(q + 1);
      }
    }
    for (const e of E) {
      const dx = x - e.x, dy = y - e.y, d2 = dx * dx + dy * dy + 1;
      if (d2 < 2500) {
        fx += (dx / d2) * 60;
        fy += (dy / d2) * 60;
      }
    }
    return (fx > 0.3 ? 8 : fx < -0.3 ? 4 : 0) | (fy > 0.3 ? 2 : fy < -0.3 ? 1 : 0);
  };
}

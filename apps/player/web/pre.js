// Browser build: lets a page pass command-line arguments to px-player, e.g.
//   px-player.html?arg=--frames&arg=930&arg=--input-script&arg=/examples/platformer/tests/full_run.input
// Used by automated browser tests; normal visitors pass no arguments.
Module['arguments'] = Module['arguments'] || [];
if (typeof location !== 'undefined') {
  for (const value of new URLSearchParams(location.search).getAll('arg')) {
    Module['arguments'].push(value);
  }
}

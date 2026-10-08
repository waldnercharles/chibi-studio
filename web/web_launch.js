// The same launch options as native, scoped to explicit URL parameters.
// Options a workspace script already chose, such as its project root, take precedence.
if (typeof window !== 'undefined') {
    const launch = new URLSearchParams(window.location.search);
    const args = Module['arguments'] || [];
    if (launch.has('play')) args.push('--play');
    for (const name of ['project-root', 'scene', 'scenario', 'frames', 'fixed-dt']) {
        if (launch.has(name) && !args.includes('--' + name)) args.push('--' + name, launch.get(name));
    }
    Module['arguments'] = args;
}

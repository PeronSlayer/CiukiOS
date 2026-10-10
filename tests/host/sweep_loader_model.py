"""Bounded BOOT.CFG extension to the existing scripts/test loader model."""
from loader_model import boot_options
from physical import cfg_selector


def config(options, menu=None, fw_cfg=None):
    if not isinstance(options, str) or not options.isascii() or len(options) > 127 or '\0' in options:
        raise ValueError('invalid BOOT.CFG extent')
    request = None; ordinary = []
    for line in options.splitlines():
        stripped = line.lstrip(' ')
        if stripped.startswith('probe='):
            if request is not None: raise ValueError('duplicate probe line')
            request = cfg_selector(stripped[6:])
        else: ordinary.append(line)
    result = boot_options('\n'.join(ordinary))
    selected = request; source = 'cfg' if selected else 'menu'
    if fw_cfg is not None: selected = fw_cfg; source = 'fw_cfg'
    elif menu is not None:
        if menu.lower() not in ('n', 's'): raise ValueError('invalid menu override')
        selected = None; source = 'menu'; result['safe'] |= menu.lower() == 's'
    return {**result, 'selection': selected, 'source': source}

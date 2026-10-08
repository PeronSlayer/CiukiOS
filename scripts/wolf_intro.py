"""Wait for recognized Wolf intro states without treating fades as failures."""
import time


def wait_for_menu(capture, menu, intro, acknowledge, *, timeout=90, max_acknowledgements=4,
                  on_acknowledge=None, clock=time.monotonic, sleep=time.sleep):
    """Return the exact menu after bounded, nonduplicated page acknowledgements.

    Each recognized page permits one key and starts a new timeout. Frames
    failing both exact recognizers consume that timeout without input. The
    acknowledgement limit bounds total time to at most (limit+1)*timeout.
    """
    assert timeout > 0 and max_acknowledgements > 0
    started = clock()
    deadline = started + timeout
    acknowledged = []
    last_identity = None
    samples = 0
    last_menu = last_intro = None
    while clock() < deadline:
        frame = capture()
        samples += 1
        last_menu = menu(frame)
        if last_menu['matched']:
            return frame, dict(menu=last_menu, acknowledgements=acknowledged,
                               samples=samples, seconds=round(clock() - started, 3))
        last_intro = intro(frame)
        if last_intro['matched']:
            identity = (last_intro.get('screen'), last_intro.get('picture'))
            assert identity != (None, None), f'Intro recognizer omitted page identity: {last_intro}'
            if identity != last_identity:
                assert len(acknowledged) < max_acknowledgements, (
                    f'Wolf intro acknowledgement limit reached: {acknowledged}; next={last_intro}')
                evidence = dict(page=last_intro, seconds=round(clock() - started, 3),
                                number=len(acknowledged) + 1)
                acknowledged.append(evidence)
                if on_acknowledge is not None:
                    on_acknowledge(frame, evidence)
                acknowledge()
                last_identity = identity
                deadline = clock() + timeout
        sleep(.25)
    raise AssertionError(f'Wolf intro/menu state timed out after {clock() - started:.3f}s: '
                         f'acknowledgements={acknowledged}; intro={last_intro}; menu={last_menu}')

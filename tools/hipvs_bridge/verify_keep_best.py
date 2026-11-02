"""Model the C `keep_best` helper to prove it returns the exact top-k.

The C bench cannot be compiled on this machine (no ROCm, no C compiler), so the
one part whose correctness is not obvious -- the bounded insertion used to build
ground truth -- is re-implemented here and compared against a naive sort over many
random cases, including heavy duplication to probe tie handling.
"""

import random


def keep_best(best, count, capacity, dist, index):
    if count == capacity and dist >= best[capacity - 1][0]:
        return count
    pos = count if count < capacity else capacity - 1
    while pos > 0 and best[pos - 1][0] > dist:
        best[pos] = best[pos - 1]
        pos -= 1
    best[pos] = (dist, index)
    if count < capacity:
        count += 1
    return count


def run(values, capacity):
    best = [(0.0, -1)] * capacity
    count = 0
    for index, dist in enumerate(values):
        count = keep_best(best, count, capacity, dist, index)
    assert count == min(capacity, len(values)), count
    return [item[0] for item in best[:count]]


def main():
    rng = random.Random(20261102)
    cases = 0
    for _ in range(400):
        size = rng.randint(1, 200)
        capacity = rng.randint(1, min(30, size))
        values = [rng.random() for _ in range(size)]
        got = run(values, capacity)
        want = sorted(values)[:capacity]
        assert got == want, (size, capacity, got[:5], want[:5])
        # the returned array must also be ascending, which the recall loop relies on
        assert got == sorted(got)
        cases += 1

    # duplicates: ties are allowed to resolve arbitrarily, but the multiset of
    # distances kept must still be the smallest `capacity` distances.
    for _ in range(200):
        size = rng.randint(2, 120)
        capacity = rng.randint(1, min(20, size))
        values = [float(rng.randint(0, 4)) for _ in range(size)]
        got = run(values, capacity)
        assert got == sorted(values)[:capacity], (values, got)
        cases += 1

    print(f"keep_best verified on {cases} cases: exact top-k, ascending, ties safe")


if __name__ == "__main__":
    main()

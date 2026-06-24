"""
Compare pgnexport output against python-chess reference for all test .pgn files.
Extracts SAN move sequences from both and compares semantically.
"""
import subprocess
import sys
import os
import io
import chess.pgn

PGNEXPORT = os.path.join(os.path.dirname(__file__), '..', 'build', 'pgnexport.exe')
TEST_DIR = os.path.dirname(__file__)

def extract_san_sequence(pgn_text):
    """Parse PGN with python-chess and extract flat list of SAN moves (excl comments)."""
    game = chess.pgn.read_game(io.StringIO(pgn_text))
    if game is None:
        return None, None
    moves = []
    node = game
    while node.variations:
        move = node.variation(0)
        # Skip move numbers - we just want SAN
        san = move.san()
        if san:
            moves.append(san)
        node = move
    return moves, game.headers.get('Result', '*')

def main():
    if not os.path.exists(PGNEXPORT):
        print(f"ERROR: pgnexport not found at {PGNEXPORT}")
        sys.exit(1)

    pgn_files = sorted(f for f in os.listdir(TEST_DIR) if f.endswith('.pgn'))
    passed = 0
    failed = 0
    skipped = 0
    failures = []

    for fname in pgn_files:
        path = os.path.join(TEST_DIR, fname)

        # 1) Get reference from python-chess
        with open(path, encoding='utf-8') as f:
            ref_moves, ref_result = extract_san_sequence(f.read()) or (None, None)

        if ref_moves is None:
            print(f"  SKIP  {fname}: python-chess couldn't parse original")
            skipped += 1
            continue

        # 2) Run pgnexport
        result = subprocess.run(
            [PGNEXPORT, path],
            capture_output=True, text=True, timeout=30
        )
        if result.returncode != 0:
            print(f"  SKIP  {fname}: pgnexport returned {result.returncode}")
            skipped += 1
            continue

        # 3) Parse pgnexport output with python-chess
        test_moves, test_result = extract_san_sequence(result.stdout) or (None, None)
        if test_moves is None:
            print(f"  FAIL  {fname}: pgnexport output unparseable by python-chess")
            failed += 1
            failures.append(fname)
            continue

        # 4) Compare moves and result
        moves_match = (ref_moves == test_moves)
        result_match = (ref_result == test_result)

        if moves_match and result_match:
            print(f"  PASS  {fname}")
            passed += 1
        else:
            print(f"  FAIL  {fname}")
            if not moves_match:
                print(f"    Moves differ:")
                print(f"      python-chess ({len(ref_moves)}): {' '.join(ref_moves[:10])}...")
                print(f"      pgnexport   ({len(test_moves)}): {' '.join(test_moves[:10])}...")
            if not result_match:
                print(f"    Result: python={ref_result} pgnexport={test_result}")
            failed += 1
            failures.append(fname)

    print(f"\n{'='*50}")
    print(f"Results: {passed} passed, {failed} failed, {skipped} skipped out of {len(pgn_files)}")
    if failures:
        print(f"Failures: {', '.join(failures)}")
    return 0 if failed == 0 else 1

if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
"""LongBench v2 evaluation for KV cache quality comparison.

Tests multi-document QA and single-document QA at various context lengths.
Multiple-choice format (A/B/C/D) for objective scoring.

Usage:
    python3 longbench_eval.py [--url URL] [--max-rows 20] [--categories multidoc_qa,single_doc_qa]
"""

import json
import re
import time
import argparse
import requests
from datasets import load_dataset


def load_longbench():
    """Load LongBench v2."""
    ds = load_dataset('THUDM/LongBench-v2', split='train')
    return list(ds)


def send_question(url, context, question, choices, max_tokens=2000):
    """Send a LongBench question to the server."""
    prompt = f"{context}\n\nQuestion: {question}\n\nChoices:\nA. {choices[0]}\nB. {choices[1]}\nC. {choices[2]}\nD. {choices[3]}\n\nAnswer with only the letter (A, B, C, or D)."

    payload = {
        "model": "qwen3.8-27b",
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": max_tokens,
        "stream": False,
        "temperature": 0,
        "top_p": 1.0,
    }

    start = time.time()
    try:
        response = requests.post(url, json=payload, timeout=3600)
        elapsed = time.time() - start
    except requests.exceptions.ReadTimeout:
        return None, 3600

    if response.status_code != 200:
        return None, elapsed

    data = response.json()
    content = data.get("choices", [{}])[0].get("message", {}).get("content", "")
    prompt_tokens = data.get("usage", {}).get("prompt_tokens", 0)
    gen_tokens = data.get("usage", {}).get("completion_tokens", 0)
    finish = data.get("choices", [{}])[0].get("finish_reason", "?")

    # Extract A/B/C/D from response
    answer = None
    # Look for standalone letter
    matches = re.findall(r'\b([ABCD])\b', content)
    if matches:
        answer = matches[-1]  # last letter mention
    elif content.strip().startswith(('A', 'B', 'C', 'D')):
        answer = content.strip()[0]

    return {
        "answer": answer,
        "content": content.strip()[:200],
        "prompt_tokens": prompt_tokens,
        "gen_tokens": gen_tokens,
        "finish": finish,
        "elapsed": elapsed,
    }, elapsed


def run_eval(url, max_rows, categories, max_context_tokens):
    """Run LongBench v2 evaluation."""
    all_samples = load_longbench()

    # Filter by category and context size
    cat_map = {
        "multidoc_qa": "Multi-Document QA",
        "single_doc_qa": "Single-Document QA",
        "code": "Code Repository Understanding",
        "in_context_learning": "Long In-context Learning",
        "dialogue": "Long-dialogue History Understanding",
        "structured": "Long Structured Data Understanding",
    }

    filtered = []
    for s in all_samples:
        domain = s["domain"]
        if domain in cat_map.values() or domain in [cat_map[c] for c in categories if c in cat_map]:
            token_est = len(s["context"]) // 4
            if token_est <= max_context_tokens:
                filtered.append(s)

    if max_rows:
        filtered = filtered[:max_rows]

    results = []
    correct = 0
    total = len(filtered)

    print(f"LongBench v2 Evaluation")
    print(f"URL: {url}")
    print(f"Categories: {categories}")
    print(f"Max context: {max_context_tokens} tokens")
    print(f"Samples: {total}")
    print(f"{'='*70}")

    for i, s in enumerate(filtered):
        expected = s["answer"]
        choices = [s["choice_A"], s["choice_B"], s["choice_C"], s["choice_D"]]
        token_est = len(s["context"]) // 4

        print(f"\n[{i+1}/{total}] {s['domain']} / {s['sub_domain']} ({s['difficulty']}, ~{token_est//1000}k tokens)")
        print(f"  Q: {s['question'][:80]}...")

        result, elapsed = send_question(url, s["context"], s["question"], choices)
        if result is None:
            print(f"  ERROR/TIMEOUT")
            results.append({"id": s["_id"], "domain": s["domain"], "expected": expected, "got": None, "correct": False, "elapsed": elapsed})
            continue

        got = result["answer"]
        is_correct = got == expected
        if is_correct:
            correct += 1

        status = "CORRECT" if is_correct else "WRONG"
        got_str = got if got else "None"
        print(f"  {status}: got={got_str} expected={expected} tokens={result['prompt_tokens']} time={elapsed:.1f}s finish={result['finish']}")

        results.append({
            "id": s["_id"],
            "domain": s["domain"],
            "sub_domain": s["sub_domain"],
            "difficulty": s["difficulty"],
            "length": s["length"],
            "expected": expected,
            "got": got,
            "correct": is_correct,
            "prompt_tokens": result["prompt_tokens"],
            "gen_tokens": result["gen_tokens"],
            "elapsed": elapsed,
            "response": result["content"],
        })

    score = correct / total * 100 if total > 0 else 0
    print(f"\n{'='*70}")
    print(f"Score: {correct}/{total} = {score:.1f}%")
    print(f"Total time: {sum(r['elapsed'] for r in results):.1f}s")

    # Per-domain breakdown
    domains = {}
    for r in results:
        d = r["domain"]
        if d not in domains:
            domains[d] = {"correct": 0, "total": 0}
        domains[d]["total"] += 1
        if r["correct"]:
            domains[d]["correct"] += 1

    print(f"\nPer-domain:")
    for d, stats in sorted(domains.items()):
        print(f"  {d}: {stats['correct']}/{stats['total']} ({stats['correct']/stats['total']*100:.0f}%)")

    with open("/tmp/longbench_results.json", "w") as f:
        json.dump({"score": score, "correct": correct, "total": total, "results": results}, f, indent=2)
    print(f"\nResults saved to /tmp/longbench_results.json")

    return score


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="LongBench v2 evaluation for ninfer")
    parser.add_argument("--url", default="http://strix.lan:8080/v1/chat/completions")
    parser.add_argument("--max-rows", type=int, default=20)
    parser.add_argument("--categories", default="multidoc_qa,single_doc_qa",
                        help="Comma-separated categories")
    parser.add_argument("--max-context-tokens", type=int, default=240000,
                        help="Max context tokens (leave room for question+output)")
    args = parser.parse_args()

    run_eval(args.url, args.max_rows, args.categories.split(","), args.max_context_tokens)

import json

log_path = r"C:\Users\arai5\.gemini\antigravity\brain\c1ae1ae3-50ec-4c68-ba88-8cb887676d3a\.system_generated\logs\transcript.jsonl"

with open(log_path, 'r', encoding='utf-8') as f:
    for line in f:
        if 'D3D11_BIND_' in line or 'D3D11_RESOURCE_MISC_' in line or 'BindFlags' in line:
            data = json.loads(line)
            content = data.get('content', '')
            if 'BindFlags' in content or 'D3D11_BIND' in content or 'MiscFlags' in content:
                print("FOUND IN CONTENT:")
                print(content[:1000]) # Print first 1000 chars


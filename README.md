# snifzig

Sniffer Zigbee passivo para Seeed Studio XIAO ESP32-C6, baseado em ESPHome.

## Funcionamento

- No arranque, tenta ligar ao Wi-Fi configurado no YAML.
- Se não conseguir, após 30 segundos abre o ponto de acesso aberto **Snifzig** com portal cativo para introduzir uma rede Wi-Fi.
- Quando a ligação à estação Wi-Fi é confirmada, o firmware desliga a rádio Wi-Fi e inicia a escuta Zigbee no canal configurado. O LED pisca três vezes rapidamente enquanto aguarda Wi-Fi e uma vez por segundo durante a captura.
- Ao fim de cada hora efetiva de captura, envia um relatório em texto ao ntfy e retoma a escuta.
- Um toque no botão BOOT pausa a captura e abre uma janela OTA de 10 minutos. No fim, o Wi-Fi desliga-se e a captura Zigbee retoma.
- As estatísticas são mantidas em RAM; não são guardados nem enviados payloads completos.

## Estatísticas por dispositivo

O firmware tenta extrair o endereço de origem dos cabeçalhos MAC Zigbee comuns (IEEE 802.15.4 versões 2003/2006) e calcula por endereço:
- número de tramas;
- RSSI médio;
- LQI médio;
- classificações indicativas.

Tramas ACK, tramas sem endereço de origem, formatos que o parser não reconhece e dispositivos além do limite de 64 endereços são agregados num único grupo “não identificadas”. A média global dá o mesmo peso a cada endereço identificado e a esse grupo único, independentemente do número de tramas. O relatório também apresenta o total de tramas, erros do recetor e médias brutas para comparação.

## Limites da interpretação

A classificação descreve os sinais recebidos no local do Snifzig, não a qualidade de todas as ligações da malha. O ponto de acesso e a estação Wi-Fi usam credenciais da configuração ESPHome; o portal é aberto apenas durante o modo de provisionamento. O captive portal guarda as credenciais introduzidas, mas uma compilação/instalação por USB com outras credenciais YAML pode substituí-las.

O LQI é um valor de 8 bits fornecido pelo driver, mas as faixas usadas para classificar “fraco/razoável/bom/excelente” são indicativas e devem ser avaliadas com os dados reais. RSSI e LQI não comprovam entrega de pacotes nem rotas.

## Configuração

A configuração de exemplo é `snifzig.yaml`. Define localmente as credenciais Wi-Fi e o URL/tópico ntfy; não coloques credenciais ou o endpoint pessoal no repositório.

# snifzig

Projeto experimental para observar indicadores da rede Zigbee 802.15.4 com um Seeed Studio XIAO ESP32-C6, em modo passivo.

## Objetivo e recolha

- Ouvir passivamente o canal Zigbee configurado, sem aderir à rede nem transmitir.
- Manter apenas contadores estatísticos em RAM; não guardar nem anexar o conteúdo das tramas.
- A cada hora de captura, ligar o Wi-Fi e publicar um resumo em texto normal no ntfy.
- Depois do envio, desligar o Wi-Fi e iniciar a hora de captura seguinte.
- Se o envio falhar, manter os contadores e tentar novamente após cinco minutos.
- Permitir OTA: manter BOOT premido durante três segundos abre uma janela Wi-Fi de dez minutos.

## Conteúdo do resumo horário

Cada notificação inclui:
- total de tramas recebidas e classificação MAC básica (beacon, dados, ACK, comando e outros);
- RSSI e LQI mínimo, médio e máximo;
- distribuição de RSSI e LQI por intervalos;
- erros de receção observados pelo driver.

Os intervalos correspondem ao tempo efetivo de captura; a pausa para ligar ao Wi-Fi e publicar o resumo fica fora da medição. O sniffer é passivo e só caracteriza o tráfego que chega ao local onde está instalado. Não mede diretamente entregas, retransmissões ou rotas entre outros dispositivos.

## Limites técnicos

- O ESP32-C6 partilha a rádio entre Wi-Fi e 802.15.4. Durante a publicação e a janela OTA, o Snifzig não ouve Zigbee; a rede Zigbee continua a funcionar.
- A classificação de tipo usa os bits do Frame Control IEEE 802.15.4. Não identifica dispositivos nem interpreta payloads cifrados.
- O LED laranja (GPIO15) pisca durante a captura e fica aceso continuamente quando o Wi-Fi está ligado na janela OTA.
- As credenciais Wi-Fi ficam em `secrets.yaml` local. O endereço ntfy deve ser definido apenas na configuração local.

## Compilar e instalar

A configuração de exemplo está em `snifzig.yaml`. Usar a configuração local com as credenciais e o tópico ntfy próprios.

Ver [docs/ROADMAP.md](docs/ROADMAP.md) para o estado e próximos passos.

import createTsanpr from './tsanpr.js';
const M = await createTsanpr();
const init = M.cwrap('anpr_initialize','string',['string']);
const readFile = M.cwrap('anpr_read_file','string',['string','string','string']);
console.log('init =', JSON.stringify(init('text;country=JP')));
console.log('text =', readFile('licensePlate.jpg','text',''));
console.log('json =', readFile('licensePlate.jpg','json',''));

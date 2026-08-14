import { createApp } from 'vue'
import { createPinia } from 'pinia'
import App from './App.vue'
import './styles.css'
import { runtimeConfig } from './config'

document.title = runtimeConfig.platformName

createApp(App).use(createPinia()).mount('#app')

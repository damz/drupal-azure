<?php

require_once dirname(__FILE__) . '/phpazure/library/Microsoft/AutoLoader.php';

// TODO: the autoloader does not work correctly and also requires this.
set_include_path(get_include_path() . PATH_SEPARATOR . dirname(__FILE__) . '/phpazure/library');

/**
 * Implementation of the Drupal Stream Wrappers interface for Azure Blob Storage.
 *
 * In addition to the base implementation provided by the PHPAzure SDK, this
 * implementation:
 *  - properly set the MIME type of the objects when saving them
 *  - put all files in a pre-selected containers
 *  - implement the Drupal additions to the stream wrapper interface (external
 *    URL, chmod, dirname, etc.).
 */
class DrupalAzureStorageStreamWrapper extends Microsoft_WindowsAzure_Storage_Blob_Stream implements DrupalStreamWrapperInterface {

  public static $storageClients = array();

  /**
   * Build a storage client.
   */
  public static function buildStorageClient($scheme) {
    if (!isset(DrupalAzureStorageStreamWrapper::$storageClients[$scheme])) {
      $storages = variable_get('azure_storage_stores', array());

      if (isset($storages[$scheme])) {
        $storage = $storages[$scheme];

        $storage_client = new Microsoft_WindowsAzure_Storage_Blob(
          $storage['storage url'],
          $storage['storage account'],
          $storage['primary key'],
          false,
          new Microsoft_WindowsAzure_RetryPolicy_retryN(10, 250)
        );

        // Store our specific configuration directly in the object.
        // Yes, I know.
        $storage_client->configuration = $storage;

        DrupalAzureStorageStreamWrapper::$storageClients[$scheme] = $storage_client;
      }
      else {
        throw new Exception(t('Invalid storage scheme @engine', array('@engine' => $scheme)));
      }
    }

    return DrupalAzureStorageStreamWrapper::$storageClients[$scheme];
  }

  /**
   * Override for Microsoft_WindowsAzure_Storage_Blob_Stream::_getStorageClient().
   *
   * Inject the storage client when needed.
   */
  public function _getStorageClient($uri) {
    if (!isset($this->_storageClient)) {
      $scheme = file_uri_scheme($uri);
      return DrupalAzureStorageStreamWrapper::buildStorageClient($scheme);
    }

    return $this->_storageClient;
  }

  /**
   * Extract container name.
   */
  protected function _getContainerName($path) {
    return $this->_getStorageClient($path)->configuration['container'];
  }

  /**
   * Extract file name.
   */
  protected function _getFileName($path) {
    return file_uri_target($path);
  }

  public function stream_close() {
    // Let the parent save the blob.
    parent::stream_close();

    $this->_getStorageClient($this->_fileName)->setBlobProperties($this->_getContainerName($this->_fileName), $this->_getFileName($this->_fileName), null, array(
      'x-ms-blob-content-type' => $this->getMimeType($this->_fileName),
    ));
  }

  public function url_stat($uri, $flags) {
    $target = file_uri_target($uri);

    // Sane default values.
    $defaults = array(
      'dev' => 0,
      'ino' => 0,
      'mode' => 0644,
      'nlink' => 0,
      'uid' => 0,
      'gid' => 0,
      'rdev' => 0,
      'size' => 0,
      'atime' => 0,
      'mtime' => 0,
      'ctime' => 0,
      'blksize' => 0,
      'blocks' => 0,
    );

    if ($target === '' || isset($this->_known_dirs[$target])) {
      // It is a directory.
      $defaults['mode'] |= 040755;
      return array_values($defaults) + $defaults;
    }
    else {
      $stat = parent::url_stat($uri, $flags);
      if ($stat !== FALSE) {
        return array_values($stat) + $stat;
      }
      else {
        // Hm. There is no blob by that name, maybe it is a directory?
        $child_blobs = $this->_getStorageClient($uri)->listBlobs($this->_getContainerName($uri), $target . '/', '/', 1);
        if ($child_blobs) {
          // It is a directory.
          $defaults['mode'] |= 0140755;
          return array_values($defaults) + $defaults;
        }
        else {
          // Really not a file.
          return false;
        }
      }
    }
  }

  public function mkdir($uri, $mode, $options) {
    // Ignore all mkdir requests. They are useless on Azure.
    return TRUE;
  }

  public function rmdir($uri, $options) {
    // Just check if the "directory" is not empty.
    $target = file_uri_target($uri);

    $child_blobs = $this->_getStorageClient($uri)->listBlobs(
      $this->_getContainerName($uri),
      $target !== '' ? $target . '/' : '',
      '/',
      1
    );
    if ($child_blobs) {
      return FALSE;
    }
    else {
      return TRUE;
    }
  }

  public function dir_opendir($uri, $options) {
    $target = file_uri_target($uri);
    $blobs = $this->_getStorageClient($uri)->listBlobs(
      $this->_getContainerName($uri),
      $target !== '' ? $target . '/' : '',
      '/'
    );

    if (is_array($blobs)) {
      $this->_blobs = array();
      $this->_known_dirs = array();

      // Post-process the results.
      foreach ($blobs as $blob) {
        if ($target !== '') {
          // Strip the directory name.
          $blob_name = preg_replace('/^' . preg_quote($target . '/', '/') . '/', '', $blob->Name);
        }
        else {
          $blob_name = $blob->Name;
        }

        if ($blob->isPrefix) {
          // Strip the trailing slash.
          $blob_name = rtrim($blob_name, '/');

          // Record that this prefix is a "directory", to optimize stream_stat().
          $this->_known_dirs[] = $blob->Name;
        }

        // Strip the directory name.
        $this->_blobs[] = $blob_name;
      }

      return TRUE;
    }
    else {
      return FALSE;
    }
  }

  public function dir_readdir() {
    $object = current($this->_blobs);
    if ($object !== false) {
      next($this->_blobs);
      return $object;
    }
    return false;
  }

  public function getUri() {
    return $this->_fileName;
  }

  public function setUri($uri) {
    $this->_fileName = $uri;
  }

  public function getExternalUrl() {
    // If the file is a styles derivative, treat it differently.
    $target = file_uri_target($this->_fileName);
    $parts = explode('/', $target);
    $first_part = array_shift($parts);
    if ($first_part == 'styles') {
      return url('azure/generate/' . implode('/', $parts));
    }
    else {
      return $this->getRealExternalUrl();
    }
  }

  public function getRealExternalUrl() {
    return $this->_getStorageClient($this->_fileName)->getBaseUrl() . '/' . $this->_getContainerName($this->_fileName) . '/' . $this->_getFileName($this->_fileName);
  }

  public static function getMimeType($uri, $mapping = NULL) {
    return DrupalPublicStreamWrapper::getMimeType($uri, $mapping);
  }

  public function chmod($mode) {
    // TODO: implement.
    return TRUE;
  }

  public function realpath() {
    // TODO: If called as temporary://, we should return a realpath.
    return FALSE;
  }

  public function dirname($uri = NULL) {
    list($scheme, $target) = explode('://', $uri, 2);
    $target  = file_uri_target($uri);
    $dirname = dirname($target);

    if ($dirname == '.') {
      $dirname = '';
    }

    return $scheme . '://' . $dirname;
  }

  public function stream_lock($operation) {
    return TRUE;
  }

}
